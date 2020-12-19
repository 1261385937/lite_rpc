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
		boost::asio::steady_timer timer_;
		boost::asio::strand<boost::asio::io_context::executor_type> strand_;
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

		std::unordered_set<std::string> subscribe_keys_;
#ifdef LITERPC_ENABLE_ZSTD
		ZSTD_CCtx* cctx_;
#endif
		std::mutex cctx_mtx_;
	public:
		explicit session(boost::asio::io_context& io_context, tcp::socket socket, const Resource& rc, compress_detail& d, rpc_server<Resource>* s)
			: rc_(rc),
			socket_(std::move(socket)),
			timer_(io_context),
			strand_(io_context.get_executor()),
			io_context_(io_context),
			compress_(d),
			server_(s)
		{
			buf_.resize(4_k);
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
			boost::asio::spawn(strand_, [this, self](boost::asio::yield_context yield) {
				yield_ = &yield;
				try {
					char data[sizeof(header)];
					for (;;) {
						timer_.expires_from_now(std::chrono::seconds(10));
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
				catch (std::exception& ) {
					//SPDLOG_ERROR("get exception:{}, so server close the connection, session address:{}", e.what(), (uint64_t)this);
					close_socket();
					boost::system::error_code ignored_ec;
					timer_.cancel(ignored_ec);

					//self remove subscribe which in rpc_server sub_conn_
					for (const auto& key : subscribe_keys_) {
						server_->remove_subscribe(key, this);
					}
				}
			});

			boost::asio::spawn(strand_, [this, self](boost::asio::yield_context yield) {
				while (socket_.is_open()) {
					boost::system::error_code ignored_ec;
					timer_.async_wait(yield[ignored_ec]);
					if (timer_.expires_from_now() <= std::chrono::seconds(0)) {
						close_socket();
						//SPDLOG_INFO("the connection is quiet over 10s, server close the connection, session address:{}", (uint64_t)this);
					}
				}
				//SPDLOG_INFO("timer quit, session address:{}", (uint64_t)this);
			});
		}

		template<request_type ReqType = request_type::req_res, typename String, typename BodyType>
		void respond(String&& key, BodyType&& data, uint64_t msg_id) {
			this->write<ReqType>(std::forward<String>(key), std::forward<BodyType>(data), msg_id);
		}

		template<request_type ReqType = request_type::req_res, typename BodyType>
		void respond(BodyType&& data, uint64_t msg_id) {
			this->write<ReqType>(std::string{}, std::forward<BodyType>(data), msg_id);
		}

		template<typename BodyType>
		void inner_respond(BodyType&& data) {
			this->inner_write(std::forward<BodyType>(data), msg_id_, *yield_);
		}

		void inner_respond() {
			this->inner_write(std::string{}, msg_id_, *yield_);
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
			if (head.req_type == request_type::sub_pub) {
				subscribe_keys_.emplace(name);
				server_->add_subscribe(std::move(name), this);
				return;
			}

			//req_res. head->name_length is 0
#ifdef LITERPC_ENABLE_ZSTD
			auto buf = decompress(compress_, { buf_.data() + head.name_length, head.body_length }, head.decompress_length);
#else
			auto buf = std::string_view{ buf_.data() + head.name_length, head.body_length };
#endif
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

		template<request_type ReqType, typename String, typename BodyType>
		void write(String&& key, BodyType&& body, uint64_t msg_id) { //maybe multi_thread operator
			using T = std::remove_cv_t<std::remove_reference_t<decltype(body)>>;
			static_assert(!std::is_pointer_v<T>, "BodyType can not be a pointer");
			size_t decompress_length = 0;
			size_t body_length = 0;
			packet pa{};

			if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::vector<char>> || std::is_same_v<T, std::string_view>) { //do not need serialize
				decompress_length = body.size();
#ifdef LITERPC_ENABLE_ZSTD
				if (decompress_length >= COMPRESS_THRESHOLD) {//need compress
					body_length = compress_with_mutex(compress_, body, pa.inner_buf);
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
			else { //need serialize
				auto seri = serialize(std::forward<BodyType>(body));
				decompress_length = seri.size();
#ifdef LITERPC_ENABLE_ZSTD
				if (decompress_length >= COMPRESS_THRESHOLD) {//need compress
					body_length = compress_with_mutex(compress_, seri, pa.inner_buf);
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
			make_head_v1_0(pa.head, (uint32_t)decompress_length, (uint32_t)body_length, msg_id, ReqType, (uint16_t)key.length());
			pa.name = std::move(key);

			std::unique_lock<std::mutex> lock(mtx_);
			send_queue_.emplace_back(std::move(pa));
			if (send_queue_.size() > 1) {
				return; //write was begined, it will send all send_queue_ data in async_write callback step by step.
			}
			lock.unlock();
			send();
		}

		template<typename BodyType>
		void inner_write(BodyType&& body, uint64_t msg_id, boost::asio::yield_context& yield) {
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
		std::unordered_map<std::string, conn_set> sub_conn_;
		std::mutex sub_conn_mtx_;
	public:
		rpc_server(uint16_t port, const std::vector<Resource>& rc = std::vector<Resource>{})
			:port_(port), session_rc_(rc)
		{}

		template<typename BodyType>
		void publish(const std::string& key, const BodyType& body) {
			// do not get conn_set, then publish. Or maybe get invalid sess.
			// do this under sub_conn_mtx_
			std::unique_lock<std::mutex> lock(sub_conn_mtx_);
			auto iter = sub_conn_.find(key);
			if (iter == sub_conn_.end()) { //none
				return;
			}

			for (const auto& sess : iter->second) {
				sess->template respond<request_type::sub_pub>(key, body, 0);
			}
		}

		void add_subscribe(std::string&& key, session<Resource>* sess) {
			std::unique_lock<std::mutex> lock(sub_conn_mtx_);
			auto iter = sub_conn_.find(key);
			if (iter != sub_conn_.end()) {
				iter->second.emplace(sess);
			}
			else {
				sub_conn_.emplace(std::move(key), conn_set{ sess });
			}
		}

		void remove_subscribe(const std::string& key, session<Resource>* sess) {
			std::unique_lock<std::mutex> lock(sub_conn_mtx_);
			auto iter = sub_conn_.find(key);
			if (iter != sub_conn_.end()) {
				iter->second.erase(sess);
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
							sess.lock()->inner_respond();
						}
						else {
							sess.lock()->inner_respond(handler(std::forward<decltype(args)>(args)...));
						}
					}
					else {
						if constexpr (std::is_same_v<return_type, void>) {
							(*obj.*handler)(std::forward<decltype(args)>(args)...);
							sess.lock()->inner_respond();
						}
						else {
							sess.lock()->inner_respond((*obj.*handler)(std::forward<decltype(args)>(args)...));
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