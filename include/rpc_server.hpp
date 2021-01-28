#pragma once
#include <thread>
#include <memory>
#include <atomic>
#include <deque>
#include <mutex>
#include "boost/asio/ip/tcp.hpp"
#include "boost/asio/spawn.hpp"
#include "boost/asio/steady_timer.hpp"
#include "boost/asio/write.hpp"
#include "boost/asio/read.hpp"

#include "rpc_common.hpp"
#include "io_context_pool.hpp"
#include "rpc_meta.hpp"

namespace lite_rpc {
	using boost::asio::ip::tcp;

	//this resource use for non mutex
	struct empty_resource {};

	template<typename Resource> class rpc_server;

	template<typename Resource = empty_resource>
	class session : public std::enable_shared_from_this<session<Resource>> {
	public:
		const Resource& rc_;
	private:
		tcp::socket socket_;
		boost::asio::steady_timer kick_timer_;
		//boost::asio::steady_timer notify_timer_;
		//bool wait_now_ = true;
		boost::asio::io_context& io_context_;
		compress_detail& compress_;
		rpc_server<Resource>* server_;

		std::vector<char> buf_;
		struct packet {
			header head;
			std::string name;
			std::string inner_buf;
			uint32_t buf_size;
			char* ext_buf;
		};
		std::deque<packet> send_queue_;
		std::mutex mtx_;

		uint64_t msg_id_;
		boost::asio::yield_context* yield_;

		std::unordered_map<size_t, std::unordered_set<size_t>> subscribe_keys_hash_;
		
#ifdef LITERPC_ENABLE_ZSTD
		ZSTD_CCtx* cctx_;
		std::mutex cctx_mtx_;
#endif
	public:
		explicit session(boost::asio::io_context& io_context, tcp::socket socket, const Resource& rc, compress_detail& d, rpc_server<Resource>* s)
			: rc_(rc),
			socket_(std::move(socket)),
			kick_timer_(io_context),
			//notify_timer_(io_context),
			io_context_(io_context),
			compress_(d),
			server_(s)
		{
			buf_.resize(4_k);
			//notify_timer_.expires_at(std::chrono::steady_clock::time_point::max());
#ifdef LITERPC_ENABLE_ZSTD
			cctx_ = ZSTD_createCCtx();
#endif
		}

		~session() {
#ifdef LITERPC_ENABLE_ZSTD
			if (cctx_) {
				ZSTD_freeCCtx(cctx_);
			}
#endif
		}

		void go() {
			//SPDLOG_INFO("get connection");
			auto self(this->shared_from_this());
			boost::asio::spawn(io_context_, [this, self](boost::asio::yield_context yield) {
				yield_ = &yield;
				try {
					char data[sizeof(header)];
					for (;;) {
						kick_timer_.expires_from_now(std::chrono::seconds(10));
						boost::asio::async_read(socket_, boost::asio::buffer(data, sizeof(header)), yield);
						auto head = (header*)data;
						msg_id_ = head->msg_id;
						auto length = head->name_length + head->body_length;
						if (buf_.size() < length) {
							buf_.resize(length);
						}
						if (length > 0) { //keepalived has no body
							boost::asio::async_read(socket_, boost::asio::buffer(buf_.data(), length), yield);
						}
						deal(*head);
					}
				}
				catch (std::exception&) {
					//SPDLOG_ERROR("get exception:{}, so server close the connection, session address:{}", e.what(), (uint64_t)this);
					close_socket();
					boost::system::error_code ignored_ec;
					kick_timer_.cancel(ignored_ec);
					//notify_timer_.cancel(ignored_ec);

					//self remove subscribe which in rpc_server sub_conn_
					for (const auto& pair : subscribe_keys_hash_) {
						for (const auto& tag_hash : pair.second) {
							server_->remove_subscribe(pair.first, tag_hash, this);
						}
					}
				}
			});

			boost::asio::spawn(io_context_, [this, self](boost::asio::yield_context yield) {
				boost::system::error_code ignored_ec;
				while (socket_.is_open()) {
					kick_timer_.async_wait(yield[ignored_ec]);
					if (kick_timer_.expires_from_now() <= std::chrono::seconds(0)) {
						close_socket();
						//SPDLOG_INFO("the connection is quiet over 10s, server close the connection, session address:{}", (uint64_t)this);
					}
				}
				//SPDLOG_INFO("timer quit, session address:{}", (uint64_t)this);
			});

			/*boost::asio::spawn(io_context_, [this, self](boost::asio::yield_context yield) {
				boost::system::error_code ignored_ec;
				while (socket_.is_open()) {
					std::unique_lock<std::mutex> l(mtx_);
					if (send_queue_.empty()) {
						l.unlock();
						wait_now_ = true;
						notify_timer_.async_wait(yield[ignored_ec]);
					}
					else {
						l.unlock();
						coro_send(yield, ignored_ec);
						wait_now_ = false;
					}
				}
			});*/
		}

		template<typename String_top, typename String_tag, typename BodyType>
		void publish(String_top&& topic, String_tag&& tag, BodyType&& body) {
			this->write<request_type::sub_pub>(std::forward<String_top>(topic), std::forward<String_tag>(tag), std::forward<BodyType>(body), 0);
		}

		template<typename BodyType>
		void respond(BodyType&& data, uint64_t msg_id) {
			this->write<request_type::req_res>(std::string{}, std::string{}, std::forward<BodyType>(data), msg_id);
		}

		template<typename BodyType>
		void coro_respond(BodyType&& data) {
			this->coro_write(std::forward<BodyType>(data), msg_id_, *yield_);
		}

		void coro_respond() {
			this->coro_write(std::string{}, msg_id_, *yield_);
		}

		auto get_msg_id() {
			return msg_id_;
		}

	private:
		void deal(const header& head) {
			if (head.req_type == request_type::keepalived) {
				return; //no response
			}

			auto name = std::string(buf_.data(), head.name_length);
#ifdef LITERPC_ENABLE_ZSTD
			auto buf = decompress(compress_, { buf_.data() + head.name_length, head.body_length }, head.decompress_length);
#else
			auto buf = std::string_view{ buf_.data() + head.name_length, head.body_length };
#endif
			//sub_pub
			if (head.req_type == request_type::sub_pub) {
				auto topic_hash = std::hash<std::string>()(name);
				auto tags_hash = split_tag_hash(buf);
				server_->add_subscribe(topic_hash, tags_hash, this);

				auto iter = subscribe_keys_hash_.find(topic_hash);
				if (iter != subscribe_keys_hash_.end()) {
					for (const auto& tag_hash : tags_hash) {
						iter->second.emplace(tag_hash);
					}
				}
				else {
					std::unordered_set<size_t> tas;
					for (const auto& tag_hash : tags_hash) {
						tas.emplace(tag_hash);
					}
					subscribe_keys_hash_.emplace(topic_hash, std::move(tas));
				}
				return;
			}

			//cancel_sub_pub
			if (head.req_type == request_type::cancel_sub_pub) {
				auto topic_hash = std::hash<std::string>()(name);
				auto tags_hash = split_tag_hash(buf);
				for (const auto& tag_hash : tags_hash) {
					server_->remove_subscribe(topic_hash, tag_hash, this);
				}

				auto iter_topic = subscribe_keys_hash_.find(topic_hash);
				if (iter_topic == subscribe_keys_hash_.end()) {
					return;
				}

				for (const auto& tag_hash : tags_hash) {
					iter_topic->second.erase(tag_hash);
				}
				if (iter_topic->second.empty()) {
					subscribe_keys_hash_.erase(iter_topic);				
				}
				return;
			}

			//req_res.
			auto iter = server_->handler_map_.find(name);
			if (iter != server_->handler_map_.end()) {
				iter->second(this->shared_from_this(), rc_, buf);
			}
			else {
				//SPDLOG_INFO("method:{} not register", name);
			}
		}

		void close_socket() {
			boost::system::error_code ignored_ec;
			socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored_ec);
			socket_.close(ignored_ec);
		}

		void send() //for one session ,producer maybe more than one, but consumer must be only one.
		{
			std::unique_lock<std::mutex> l(mtx_);
			auto& msg = send_queue_.front();
			l.unlock();

			std::vector<boost::asio::const_buffer> write_buffers;
			write_buffers.emplace_back(boost::asio::buffer((char*)&msg.head, sizeof(header)));
			if (!msg.name.empty()) {
				write_buffers.emplace_back(boost::asio::buffer(msg.name.data(), msg.name.length()));
			}
			if (msg.ext_buf != nullptr) {//ext serialize buf
				write_buffers.emplace_back(boost::asio::buffer(msg.ext_buf, msg.buf_size));
			}
			else if (!msg.inner_buf.empty()) {
				write_buffers.emplace_back(boost::asio::buffer(msg.inner_buf.data(), msg.buf_size));
			}

			boost::asio::async_write(socket_, write_buffers, [this, self = this->shared_from_this()](boost::system::error_code ec, std::size_t) {
				if (ec) {
					close_socket();
					//SPDLOG_ERROR("async_write error:{}, close the connection, session address:{}", ec.message(), (uint64_t)this);
					return;
				}

				std::unique_lock<std::mutex> lock(mtx_);
				if (send_queue_.front().ext_buf != nullptr) { //need free extra buf
					::free(send_queue_.front().ext_buf);
				}
				send_queue_.pop_front();
				if (!send_queue_.empty()) {
					lock.unlock();
					this->send();
				}
			});
		}

		void coro_send(boost::asio::yield_context& yield, boost::system::error_code& ec) //for one session ,producer maybe more than one, but consumer must be only one.
		{
			std::unique_lock<std::mutex> l(mtx_);
			auto& msg = send_queue_.front();
			l.unlock();

			std::vector<boost::asio::const_buffer> write_buffers;
			write_buffers.emplace_back(boost::asio::buffer((char*)&msg.head, sizeof(header)));
			if (!msg.name.empty()) {
				write_buffers.emplace_back(boost::asio::buffer(msg.name.data(), msg.name.length()));
			}
			if (msg.ext_buf != nullptr) {//ext serialize buf
				write_buffers.emplace_back(boost::asio::buffer(msg.ext_buf, msg.buf_size));
			}
			else if (!msg.inner_buf.empty()) {
				write_buffers.emplace_back(boost::asio::buffer(msg.inner_buf.data(), msg.buf_size));
			}

			boost::asio::async_write(socket_, write_buffers, yield[ec]);
			if (ec) {
				close_socket();
				return;
			}

			std::unique_lock<std::mutex> lock(mtx_);
			if (send_queue_.front().ext_buf != nullptr) { //need free extra buf
				::free(send_queue_.front().ext_buf);
			}
			send_queue_.pop_front();
		}

		template<request_type ReqType, typename String_top, typename String_tag, typename BodyType>
		void write(String_top&& topic, String_tag&& tag, BodyType&& body, uint64_t msg_id) { //maybe multi_thread operator
			using T = std::remove_cv_t<std::remove_reference_t<decltype(body)>>;
			static_assert(!std::is_pointer_v<T>, "BodyType can not be a pointer");
			size_t decompress_length = 0;
			size_t body_length = 0;
			packet pa{};

			if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::vector<char>> || std::is_same_v<T, std::string_view>) { //do not need serialize
				decompress_length = body.size();
#ifdef LITERPC_ENABLE_ZSTD
				if (decompress_length >= COMPRESS_THRESHOLD) {//need compress
					body_length = compress_with_mutex(cctx_, cctx_mtx_, body, pa.inner_buf);
				}
				else {
#endif
					body_length = decompress_length; //do not need compress, the length is same.
					using T = std::remove_cv_t<std::remove_reference_t<decltype(body)>>;
					if constexpr (std::is_same_v<T, std::string>) {
						pa.inner_buf = std::move(body);
					}
					else {
						pa.inner_buf = std::string(body.data(), body.size());
					}
#ifdef LITERPC_ENABLE_ZSTD
				}
#endif
			}
			else if constexpr (lite_rpc::is_char_array_v<T>) {
				decompress_length = sizeof(body) - 1;
#ifdef LITERPC_ENABLE_ZSTD
				if (decompress_length >= COMPRESS_THRESHOLD) {//need compress
					body_length = compress_with_mutex(cctx_, cctx_mtx_, std::string_view{ body,decompress_length }, pa.inner_buf);
				}
				else {
#endif
					pa.inner_buf = body;
					body_length = pa.inner_buf.length();
#ifdef LITERPC_ENABLE_ZSTD
				}
#endif
			}
			else { //need serialize
				auto seri = serialize(std::forward<BodyType>(body));
				decompress_length = seri.size();
#ifdef LITERPC_ENABLE_ZSTD
				if (decompress_length >= COMPRESS_THRESHOLD) {//need compress
					body_length = compress_with_mutex(cctx_, cctx_mtx_, seri, pa.inner_buf);
				}
				else {
#endif
					body_length = decompress_length; //do not need compress, the length is same.
					pa.ext_buf = seri.release();
#ifdef LITERPC_ENABLE_ZSTD
				}
#endif
			}
			pa.buf_size = (uint32_t)body_length;
			make_head_v1_0(pa.head, (uint32_t)decompress_length, (uint32_t)body_length, msg_id, ReqType, (uint8_t)topic.length(), (uint16_t)tag.length());
			pa.name = std::forward<String_top>(topic) + std::forward<String_tag>(tag);

			std::unique_lock<std::mutex> lock(mtx_);
			send_queue_.emplace_back(std::move(pa));
			if (send_queue_.size() > 1) {
				return; //once write is begining, it will send all send_queue_ data  step by step.
			}
			lock.unlock();
			send();
			//notify_timer_.cancel_one();
		}

		template<typename BodyType>
		void coro_write(BodyType&& body, uint64_t msg_id, boost::asio::yield_context& yield) {
			using T = std::remove_cv_t<std::remove_reference_t<decltype(body)>>;
			static_assert(!std::is_pointer_v<T>, "BodyType can not be a pointer");
			std::string_view buf{};
			size_t decompress_length = 0;
			msgpack::sbuffer seri(0);

			if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view> || std::is_same_v<T, std::vector<char>>) { //do not need serialize
				decompress_length = body.size();
#ifdef LITERPC_ENABLE_ZSTD
				buf = compress(compress_, { body.data(), body.size() });
#else
				buf = { body.data(), body.size() };
#endif
			}
			else if constexpr (lite_rpc::is_char_array_v<T>) {
				decompress_length = sizeof(body) - 1;
#ifdef LITERPC_ENABLE_ZSTD
				buf = compress(compress_, { body, decompress_length });
#else
				buf = { body, decompress_length };
#endif
			}
			else {
				seri = serialize(std::forward<BodyType>(body));
				decompress_length = seri.size();
#ifdef LITERPC_ENABLE_ZSTD
				buf = compress(compress_, { seri.data(), seri.size() });
#else
				buf = { seri.data(), seri.size() };
#endif
			}

			header head{};
			make_head_v1_0(head, (uint32_t)decompress_length, (uint32_t)buf.size(), msg_id, request_type::req_res, 0);
			std::vector<boost::asio::const_buffer> write_buffers;
			write_buffers.emplace_back(boost::asio::buffer((char*)&head, sizeof(header)));
			if (!buf.empty()) {
				write_buffers.emplace_back(boost::asio::buffer(const_cast<char*>(buf.data()), head.body_length));
			}
			boost::system::error_code ec;
			boost::asio::async_write(socket_, write_buffers, yield[ec]);
			if (ec) {
				close_socket();
			}
		}
	};



	template<typename Resource = empty_resource>
	class rpc_server {
	public:
		std::unordered_map<std::string, std::function<void(std::weak_ptr<session<Resource>>, const Resource&, std::string_view)>> handler_map_;
	private:
		boost::asio::io_context io_context_;
		uint16_t port_{};
		const std::vector<Resource>& session_rc_;
		std::atomic<bool> run_ = true;

		using conn_set = std::unordered_set<session<Resource>*>;
		std::unordered_map<size_t, std::unordered_map<size_t, conn_set>> sub_conn_;
		std::mutex sub_conn_mtx_;
	public:
		rpc_server(uint16_t port, const std::vector<Resource>& rc = std::vector<Resource>{})
			:port_(port), session_rc_(rc)
		{}

		template<typename BodyType>
		void publish(const std::string& topic, const std::string& tag, const BodyType& body) {
			// do not get conn_set, then publish. Or maybe get invalid sess.
			// do this under sub_conn_mtx_
			std::unique_lock<std::mutex> lock(sub_conn_mtx_);
			auto iter = sub_conn_.find(std::hash<std::string>()(topic));
			if (iter == sub_conn_.end()) { //none
				return;
			}

			auto& tag_conn = iter->second;
			//tag is *
			auto tag_iter = tag_conn.find(GLOBBING_HASH);
			if (tag_iter != tag_conn.end()) {
				for (const auto& sess : tag_iter->second) {
					sess->publish(topic, std::string("*"), body);
				}
			}

			//dispatch with tag
			tag_iter = tag_conn.find(std::hash<std::string>()(tag));
			if (tag_iter == tag_conn.end()) {
				return;
			}
			for (const auto& sess : tag_iter->second) {
				sess->publish(topic, tag, body);
			}
		}

		void add_subscribe(size_t topic_hash, const std::vector<size_t>& tags_hash, session<Resource>* sess) {
			std::unique_lock<std::mutex> lock(sub_conn_mtx_);
			auto topic_iter = sub_conn_.find(topic_hash);
			if (topic_iter == sub_conn_.end()) {
				std::unordered_map<size_t, conn_set> tag_conn;
				for (const auto& tag_hash : tags_hash) {
					tag_conn.emplace(tag_hash, conn_set{ sess });
				}
				sub_conn_.emplace(topic_hash, std::move(tag_conn));
				return;
			}

			auto& tag_conn = topic_iter->second;
			for (const auto& tag_hash : tags_hash) {
				auto tag_iter = tag_conn.find(tag_hash);
				if (tag_iter != tag_conn.end()) {
					tag_iter->second.emplace(sess);
				}
				else {
					tag_conn.emplace(tag_hash, conn_set{ sess });
				}
			}
		}

		void remove_subscribe(size_t topic_hash, size_t tag_hash, session<Resource>* sess) {
			std::unique_lock<std::mutex> lock(sub_conn_mtx_);
			auto iter = sub_conn_.find(topic_hash);
			if (iter == sub_conn_.end()) {
				return;
			}

			auto& tag_conn = iter->second;
			auto iter1 = tag_conn.find(tag_hash);
			if (iter1 == tag_conn.end()) {
				return;
			}

			iter1->second.erase(sess);
			if (!iter1->second.empty()) {
				return;
			}

			tag_conn.erase(iter1);
			if (tag_conn.empty()) {
				sub_conn_.erase(iter);
			}
		}

		void run(uint32_t parallel_num = std::thread::hardware_concurrency()) {
			io_context_pool icp(parallel_num);
			icp.start();
			std::vector<compress_detail> detail((size_t)parallel_num);

			boost::asio::spawn(io_context_, [this, &icp, &detail, parallel_num](boost::asio::yield_context yield) {
				tcp::acceptor acceptor(io_context_, tcp::endpoint(tcp::v4(), port_));
				size_t index = 0;
				while (run_) {
					boost::system::error_code ec;
					auto& io = icp.get_io_context();
					tcp::socket socket(io);
					acceptor.async_accept(socket, yield[ec]);
					if (!ec) {
						if constexpr (std::is_same_v<Resource, empty_resource>) { //no special Resource
							std::make_shared<session<Resource>>(io, std::move(socket), Resource{}, detail[index], this)->go();
						}
						else {
							std::make_shared<session<Resource>>(io, std::move(socket), session_rc_[index], detail[index], this)->go();
						}
						++index;
						if (index == (size_t)parallel_num) {
							index = 0;
						}
					}
				}
			});
			boost::system::error_code ignored_ec;
			io_context_.run(ignored_ec);
		}

		void stop() {
			run_ = false;
			io_context_.stop();
		}

		template<typename Func, typename Object = void>
		void register_method(const std::string& method_name, Func&& handler, Object* obj = nullptr) {
			handler_map_[method_name] =
				[handler, obj, method_name](std::weak_ptr<session<Resource>> sess, const Resource& rc, std::string_view data) {
				using func_type = std::remove_reference_t<decltype(handler)>;
				using return_type = std::remove_cv_t< std::remove_reference_t<typename function_traits<func_type>::return_type>>;
				using req_type = typename function_traits<func_type>::nonref_tuple_args_t;
				constexpr auto arg_size = function_traits<func_type>::args_size_v;
				auto direct_response = [&handler, &sess, &obj](auto&&... args) {
					if constexpr (std::is_same_v<Object, void>) {
						if constexpr (std::is_same_v<return_type, void>) {
							handler(std::forward<decltype(args)>(args)...);
							sess.lock()->coro_respond();
						}
						else {
							sess.lock()->coro_respond(handler(std::forward<decltype(args)>(args)...));
						}
					}
					else {
						if constexpr (std::is_same_v<return_type, void>) {
							(*obj.*handler)(std::forward<decltype(args)>(args)...);
							sess.lock()->coro_respond();
						}
						else {
							sess.lock()->coro_respond((*obj.*handler)(std::forward<decltype(args)>(args)...));
						}
					}
				};
				auto user_response = [&handler, &sess, &obj](auto&&... args) {
					if constexpr (std::is_same_v<Object, void>) {
						handler(sess, std::forward<decltype(args)>(args)...);
					}
					else {
						(*obj.*handler)(sess, std::forward<decltype(args)>(args)...);
					}
				};

				try {
					if constexpr (arg_size == 0) { //no arg, here directly send response
						direct_response();
					}
					else if constexpr (arg_size == 1) { //std::weak_ptr<session<Resource>> or resource or data from client
						using arg_type_1 = std::tuple_element_t<0, req_type>;
						if constexpr (std::is_same_v<arg_type_1, std::weak_ptr<session<Resource>>>) {// user want to response to client by himself
							static_assert(std::is_same_v<return_type, void>,
								"handler function return_type must be void if args has std::weak_ptr<session<Resource>>, response to client shuold be done by user");
							user_response();
						}
						else if constexpr (std::is_same_v<arg_type_1, Resource>) { //the arg is resource, here directly send response
							direct_response(rc);
						}
						else { //the arg is data from client, here directly send response
							if constexpr (std::is_same_v<arg_type_1, std::string> || std::is_same_v<arg_type_1, std::string_view> || std::is_same_v<arg_type_1, std::vector<char>>) {
								direct_response(arg_type_1{ data.data(), data.length() });
							}
							else {
								direct_response(deserialize<arg_type_1>(data.data(), data.length()));
							}
						}
					}
					else if constexpr (arg_size == 2) {
						using arg_type_1 = std::tuple_element_t<0, req_type>;
						using arg_type_2 = std::tuple_element_t<1, req_type>;
						if constexpr (std::is_same_v<arg_type_1, std::weak_ptr<session<Resource>>> && std::is_same_v<arg_type_2, Resource>) {
							//1 std::weak_ptr<session<Resource>> and resource
							static_assert(std::is_same_v<return_type, void>,
								"handler function return_type must be void if args has std::weak_ptr<session<Resource>>, response to client shuold be done by user");
							user_response(rc);
						}
						else if constexpr (std::is_same_v<arg_type_1, std::weak_ptr<session<Resource>>> && !std::is_same_v<arg_type_2, Resource>) {
							//2 std::weak_ptr<session<Resource>> and data from client
							static_assert(std::is_same_v<return_type, void>,
								"handler function return_type must be void if args has std::weak_ptr<session<Resource>>, response to client shuold be done by user");
							if constexpr (std::is_same_v<arg_type_2, std::string> || std::is_same_v<arg_type_2, std::string_view> || std::is_same_v<arg_type_2, std::vector<char>>) {
								user_response(arg_type_2{ data.data(), data.length() });
							}
							else {
								user_response(deserialize<arg_type_2>(data.data(), data.length()));
							}
						}
						else if constexpr (std::is_same_v<arg_type_1, Resource> && !std::is_same_v<arg_type_2, std::weak_ptr<session<Resource>>>) {
							//3 resource and data from client, here directly send response
							if constexpr (std::is_same_v<arg_type_2, std::string> || std::is_same_v<arg_type_2, std::string_view> || std::is_same_v<arg_type_2, std::vector<char>>) {
								direct_response(rc, arg_type_2{ data.data(), data.length() });
							}
							else {
								direct_response(rc, deserialize<arg_type_2>(data.data(), data.length()));
							}
						}
						else {
							static_assert(always_false_v<Resource>, "args type do not match");
						}
					}
					else if constexpr (arg_size == 3) {//std::weak_ptr<session<Resource>> and resource and data from client
						using arg_type_1 = std::tuple_element_t<0, req_type>;
						using arg_type_2 = std::tuple_element_t<1, req_type>;
						using arg_type_3 = std::tuple_element_t<2, req_type>;
						static_assert(std::is_same_v<return_type, void>,
							"handler function return_type must be void if args has std::weak_ptr<session<Resource>>, response to client shuold be done by user");
						static_assert(std::is_same_v<arg_type_1, std::weak_ptr<session<Resource>>>, "first arg must be std::weak_ptr<session<Resource>>");
						static_assert(std::is_same_v<arg_type_2, Resource>, "second arg must be Resource");
						if constexpr (std::is_same_v<arg_type_3, std::string> || std::is_same_v<arg_type_3, std::string_view> || std::is_same_v<arg_type_3, std::vector<char>>) {
							user_response(rc, arg_type_3{ data.data(), data.length() });
						}
						else {
							user_response(rc, deserialize<arg_type_3>(data.data(), data.length()));
						}
					}
					else {
						static_assert(always_false_v<func_type>, "handler function arg count error");
					}
				}
				catch (const std::exception&) {
					//SPDLOG_ERROR("method_name:{} excute error:{}", method_name, e.what());
				}
			};
		}
	};
}