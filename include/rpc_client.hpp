#pragma once
#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <vector>
#include <deque>
#include <type_traits>
#include <condition_variable>

#include "boost/asio.hpp"
#include "boost/asio/steady_timer.hpp"
#include "boost/asio/spawn.hpp"

#include "rpc_common.hpp"
#include "rpc_meta.hpp"

namespace lite_rpc {

	using boost::asio::ip::tcp;

	class rpc_client :public std::enable_shared_from_this<rpc_client> {
	public:
		using connect_callback = std::function<void()>;
		using disconnect_callback = std::function<void()>;

	private:
		boost::asio::io_context ioc_;
		tcp::socket socket_{ ioc_ };
		boost::asio::io_context::work work_{ ioc_ };
		boost::asio::steady_timer timer_{ ioc_ };
		std::thread ioc_thread_;

		struct packet {
			header head;
			std::string name;
			std::string inner_buf;
			uint32_t buf_size;
			char* ext_buf;
		};
		std::mutex send_queue_mtx_;
		std::deque<packet> send_queue_;
		std::vector<char> buf_;

		std::string ip_;
		std::string port_;

		uint64_t message_id_;
		std::mutex req_cb_mtx_;
		std::unordered_map<uint64_t, std::function<void(std::string_view)>> req_cb_map_;
		std::mutex sub_cb_mtx_;
		std::unordered_map<std::string, std::function<void(std::string_view)>> sub_cb_map_;

		disconnect_callback discon_callback_;

		std::thread keepalive_thread_;
		std::atomic<bool> conn_alived_ = false;
		std::atomic<bool> run_ = true;

		int timeout_s_ = 5;

		compress_detail compress_detail_;

	public:
		rpc_client() {
			buf_.resize((size_t)8_k);
			ioc_thread_ = std::thread([this]() {
				boost::system::error_code ignored_ec;
				ioc_.run(ignored_ec);
			});

			keepalive_thread_ = std::thread([this]() {
				while (run_) {
					std::this_thread::sleep_for(std::chrono::milliseconds(1500));
					if (!conn_alived_) {
						continue;
					}
					keep_alived();
					std::this_thread::sleep_for(std::chrono::milliseconds(1500));
				}
			});
			//SPDLOG_INFO("rpc_client start");
		}

		void stop() {
			run_ = false;
			if (keepalive_thread_.joinable()) {
				keepalive_thread_.join();
			}

			close_socket();
			ioc_.stop();
			if (ioc_thread_.joinable()) {
				ioc_thread_.join();
			}
			//SPDLOG_INFO("rpc_client quit");
		}

		bool conn_alived() { return conn_alived_.load(); }

		void connect_async(std::string ip, std::string port, int timeout_s, connect_callback con_cb) {
			timeout_s_ = timeout_s;
			ip_ = std::move(ip);
			port_ = std::move(port);
			reset_socket(); //maybe connect timeout ,then async_connect again, recover socket
			boost::system::error_code ec;
			auto end_point = tcp::resolver(ioc_).resolve(ip_, port_, ec);
			if (ec) {
				//SPDLOG_ERROR("tcp::resolver error: {}", ec.message());
				return;
			}

			boost::asio::async_connect(socket_, end_point,
				[con_cb, self = shared_from_this(), this](const boost::system::error_code& ec, tcp::endpoint) {
				if (ec) { //error 
					//SPDLOG_ERROR("connect server error: {}", ec.message());
					boost::system::error_code ignored_ec; timer_.cancel(ignored_ec);
					if (discon_callback_) {
						discon_callback_();
					}
					return;
				}

				//SPDLOG_INFO("connect server ok");
				//ready for reading message from server
				boost::asio::spawn(ioc_, [this, self = shared_from_this()](boost::asio::yield_context yield) {
					try {
						char data[sizeof(header)];
						for (;;) {
							boost::asio::async_read(socket_, boost::asio::buffer(data, sizeof(header)), yield);
							auto head = (header*)data;
							auto length = head->name_length + head->body_length;
							if (buf_.size() < length) {
								buf_.resize(length);
							}
							if (length > 0) { //maybe 0 if server return no data in req_res mode
								boost::asio::async_read(socket_, boost::asio::buffer(buf_.data(), length), yield);
							}
							deal(*head);
						}
					}
					catch (std::exception&) {
						//SPDLOG_ERROR("read from server error: {}, close the connection.", e.what());
						close_socket();
						//conn_alived_ = false;
					}
				});

				conn_alived_ = true;
				boost::system::error_code ignored_ec; timer_.cancel(ignored_ec);
				if (con_cb) {
					con_cb();
				}
			});

			//initiative timeout then close socket if connect time is too long
			timer_.expires_from_now(std::chrono::seconds(timeout_s_));
			timer_.async_wait([this](const boost::system::error_code& ec) {
				if (ec) {
					return;
				}

				//SPDLOG_WARN("connect server timeout({}s), close socket", timeout_s_);
				close_socket();
			});
		}

		void set_disconnect_callback(disconnect_callback discon_cb) {
			discon_callback_ = std::move(discon_cb);
		}

		template<typename ReqType, typename Callback>
		void remote_call_async(std::string&& method_name, ReqType&& req_content, Callback&& call_back) {
			this->write(std::move(method_name), std::forward<ReqType>(req_content), this->bind_cb(std::forward<Callback>(call_back)));
		}

		//shoud call after connect ok
		template<typename Callback>
		void subscribe(std::string&& key, Callback&& call_back) {
			static_assert(!std::is_same_v<std::decay_t<Callback>, std::nullptr_t>, "subscribe call_back can not be nullptr");
			std::unique_lock<std::mutex> l(sub_cb_mtx_);
			sub_cb_map_[key] = [cb = std::move(call_back)](std::string_view data) {
				if constexpr (function_traits<std::decay_t<Callback>>::args_size_v == 1) {
					using res_type = typename function_traits<std::decay_t<Callback>>::nonref_tuple_args_t;
					using first_arg_type = std::tuple_element_t<0, res_type>;
					if constexpr (std::is_same_v<first_arg_type, std::string> || std::is_same_v<first_arg_type, std::string_view> || std::is_same_v<first_arg_type, std::vector<char>>) { //do not need serialize
						cb({ data.data(),data.length() });
					}
					else {
						try {
							cb(deserialize<first_arg_type>(data.data(), data.length()));
						}
						catch (const std::invalid_argument&) {
							//SPDLOG_ERROR(e.what());
						}
					}
				}
				else if constexpr (function_traits<std::decay_t<Callback>>::args_size_v == 0) {
					cb();
				}
				else {
					static_assert(always_false_v<Callback>, "sub callback function arg count can only 1 or 0");
				}
			};
			l.unlock();
			this->write<request_type::sub_pub>(std::move(key), std::string{}, 0);
		}

		//for reconnect
		void re_subscribe() {
			std::lock_guard<std::mutex> l(sub_cb_mtx_);
			for (const auto& sub : sub_cb_map_) {
				const auto& key = sub.first;
				write<request_type::sub_pub>(key, std::string{}, 0);
			}
			send();
		}

	private:

		template<request_type ReqType = request_type::req_res, typename String, typename BodyType>
		void write(String&& name, BodyType&& body, uint64_t msg_id) { //maybe multi_thread operator
			using T = std::remove_cv_t<std::remove_reference_t<decltype(body)>>;
			static_assert(!std::is_pointer_v<T>, "BodyType can not be a pointer");
			size_t decompress_length = 0;
			size_t body_length = 0;
			packet pa{};

			if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::vector<char>> || std::is_same_v<T, std::string_view>) { //do not need serialize
				decompress_length = body.size();
#ifdef LITERPC_ENABLE_ZSTD
				if (decompress_length >= COMPRESS_THRESHOLD) {//need compress
					body_length = compress_with_mutex(compress_detail_.cctx, compress_detail_.compress_mtx, body, pa.inner_buf);
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
					body_length = compress_with_mutex(compress_detail_.cctx, compress_detail_.compress_mtx, std::string_view{ body,decompress_length }, pa.inner_buf);
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
					body_length = compress_with_mutex(compress_detail_.cctx, compress_detail_.compress_mtx, seri, pa.inner_buf);
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
			make_head_v1_0(pa.head, (uint32_t)decompress_length, (uint32_t)body_length, msg_id, ReqType, (uint16_t)name.length());
			pa.name = std::move(name);

			std::unique_lock<std::mutex> lock(send_queue_mtx_);
			send_queue_.emplace_back(std::move(pa));
			if (send_queue_.size() > 1) {
				return; //write was begined, it will send all send_queue_ data in async_write callback step by step.
			}
			lock.unlock();
			send();
		}

		template<typename Callback>
		uint64_t bind_cb(Callback&& call_back) {
			std::lock_guard<std::mutex> l(req_cb_mtx_);
			if constexpr (!std::is_same_v<std::decay_t<Callback>, std::nullptr_t>) {
				req_cb_map_[message_id_] = [cb = std::forward<Callback>(call_back)](std::string_view data) {
					if constexpr (function_traits<std::decay_t<Callback>>::args_size_v == 1) {
						using res_type = typename function_traits<std::decay_t<Callback>>::nonref_tuple_args_t;
						using first_arg_type = std::tuple_element_t<0, res_type>;
						if constexpr (std::is_same_v<first_arg_type, std::string> || std::is_same_v<first_arg_type, std::string_view> || std::is_same_v<first_arg_type, std::vector<char>>) {
							cb({ data.data(),data.length() }); //do not need serialize
						}
						else {
							try {
								cb(deserialize<first_arg_type>(data.data(), data.length()));
							}
							catch (const std::invalid_argument&) {
								//SPDLOG_ERROR(e.what());
							}
						}
					}
					else if constexpr (function_traits<std::decay_t<Callback>>::args_size_v == 0) {
						cb();
					}
					else {
						static_assert(always_false_v<Callback>, "req callback function arg count can only 1 or 0");
					}
				};
			}

			uint64_t temp_id = message_id_;
			message_id_++;
			return temp_id;
		}

		void send() {
			std::unique_lock<std::mutex> l(send_queue_mtx_);
			auto& msg = send_queue_.front();
			l.unlock();

			std::array<boost::asio::const_buffer, 3> write_buffers;
			write_buffers[0] = boost::asio::buffer((char*)&msg.head, sizeof(header));
			if (!msg.name.empty()) {
				write_buffers[1] = boost::asio::buffer(msg.name.data(), msg.name.length());
			}
			if (msg.ext_buf != nullptr) {//ext serialize buf
				write_buffers[2] = boost::asio::buffer(msg.ext_buf, msg.buf_size);
			}
			else if (!msg.inner_buf.empty()) {
				write_buffers[2] = boost::asio::buffer(msg.inner_buf.data(), msg.buf_size);
			}

			boost::asio::async_write(socket_, write_buffers, [this, self = shared_from_this()](boost::system::error_code ec, std::size_t) {
				if (ec) {
					//SPDLOG_ERROR("write to server error: {}, close the connection. Then reconnect", ec.message());
					close_socket();
					conn_alived_ = false;
					//here send_queue_ size must be >=1, so after re_subscribe, need to send initiatively
					connect_async(ip_, port_, timeout_s_, [this]() {
						re_subscribe();
						//this->send();
					});
					return;
				}

				std::unique_lock<std::mutex> lock(send_queue_mtx_);
				//printf("send handle_type:%d ok\n", send_queue_.front().head.handle_type);
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

		void reset_socket() {
			close_socket();
			socket_ = tcp::socket{ ioc_ };
		}

		void close_socket() {
			boost::system::error_code ignored_ec;
			socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored_ec);
			socket_.close(ignored_ec);
		}

		void deal(const header& h) {
			if (h.req_type == request_type::sub_pub) {
				auto key = std::string(buf_.data(), h.name_length);
#ifdef LITERPC_ENABLE_ZSTD
				auto buf = decompress(compress_detail_, { buf_.data() + h.name_length, h.body_length }, h.decompress_length);
#else
				auto buf = std::string_view{ buf_.data() + h.name_length, h.body_length };
#endif
				std::unique_lock<std::mutex> lock(sub_cb_mtx_);
				auto iter = sub_cb_map_.find(key);
				if (iter != sub_cb_map_.end()) {
					iter->second(buf);
				}
				return;
			}

			//req_res. head->name_length is 0
			//Keepalived no response
#ifdef LITERPC_ENABLE_ZSTD
			auto buf = decompress(compress_detail_, { buf_.data(), h.body_length }, h.decompress_length);
#else
			auto buf = std::string_view{ buf_.data(), h.body_length };
#endif
			auto id = h.msg_id;
			std::unique_lock<std::mutex> lock(req_cb_mtx_);
			auto iter = req_cb_map_.find(id);
			if (iter == req_cb_map_.end()) {
				return;
			}
			auto cb = std::move(iter->second);
			req_cb_map_.erase(id);
			lock.unlock();

			cb(buf);
		}

		void keep_alived() {
			write<request_type::keepalived>(std::string{}, std::string{}, 0);
		}
	};

}
