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
			bool need_free = true;
		};
		std::deque<packet> send_queue_;
		std::mutex mtx_;

		uint64_t msg_id_{};
		boost::asio::yield_context* yield_{};

		std::unordered_map<size_t, std::unordered_set<size_t>> subscribe_keys_hash_;
		std::unordered_map<std::string, std::string> session_cache_;
		std::atomic<bool> enable_publish_ = false;

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
			buf_.resize(1_k);
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
		}

		void publish(packet&& p) {
			this->publish_write(std::move(p));
		}

		void publish(const packet& p) {
			this->publish_write(p);
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

		void add_session_cache(std::string key, std::string value) {
			session_cache_.emplace(std::move(key), std::move(value));
		}

		auto& get_session_cache() {
			return session_cache_;
		}

		void enable_publish() {
			enable_publish_ = true;
		}

		void disable_publish() {
			enable_publish_ = false;
		}

		bool publish_enable_status() {
			return enable_publish_.load();
		}

		template<request_type ReqType, typename String_top, typename String_tag, typename BodyType>
		static auto make_packet(String_top&& topic, String_tag&& tag, BodyType&& body, uint64_t msg_id) {
			using T = std::remove_cv_t<std::remove_reference_t<decltype(body)>>;
			static_assert(!std::is_pointer_v<T>, "BodyType can not be a pointer");
			size_t decompress_length = 0;
			size_t body_length = 0;
			packet pa{};

			thread_local compress_detail deatil{};
			if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::vector<char>> || std::is_same_v<T, std::string_view>) { //do not need serialize
				decompress_length = body.size();
#ifdef LITERPC_ENABLE_ZSTD
				if (decompress_length >= COMPRESS_THRESHOLD) {//need compress
					body_length = compress(deatil, body, pa.inner_buf);
				}
				else {
#endif
					body_length = decompress_length; //do not need compress, the length is same.
					using T = std::remove_cv_t<std::remove_reference_t<decltype(body)>>;
					if constexpr (std::is_same_v<T, std::string>) {
						pa.inner_buf = std::forward<BodyType>(body);
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
					body_length = compress(deatil, std::string_view{ body, decompress_length }, pa.inner_buf);
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
					body_length = compress(deatil, seri, pa.inner_buf);
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
			return pa;
		}

		void close_socket() {
			boost::system::error_code ignored_ec;
			socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored_ec);
			socket_.close(ignored_ec);
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
				auto& pack = send_queue_.front();
				if (pack.ext_buf != nullptr && pack.need_free) { //need free extra buf
					::free(pack.ext_buf);
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
			auto& pack = send_queue_.front();
			if (pack.ext_buf != nullptr && pack.need_free) { //need free extra buf
				::free(pack.ext_buf);
			}
			send_queue_.pop_front();
		}

		template<request_type ReqType, typename String_top, typename String_tag, typename BodyType>
		void write(String_top&& topic, String_tag&& tag, BodyType&& body, uint64_t msg_id) { //maybe multi_thread operator
			auto pa = this->make_packet<ReqType>(std::forward<String_top>(topic), std::forward<String_tag>(tag), std::forward<BodyType>(body), msg_id);
			std::unique_lock<std::mutex> lock(mtx_);
			send_queue_.emplace_back(std::move(pa));
			if (send_queue_.size() > 1) {
				return; //once write is begining, it will send all send_queue_ data  step by step.
			}
			lock.unlock();
			send();
			//notify_timer_.cancel_one();
		}

		//avoid serialize and compress many times
		template<typename Packet>
		void publish_write(Packet&& pa) {
			std::unique_lock<std::mutex> lock(mtx_);
			send_queue_.emplace_back(std::forward<Packet>(pa));
			if (send_queue_.size() > 1) {
				return; //once write is begining, it will send all send_queue_ data  step by step.
			}
			lock.unlock();
			send();
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
}