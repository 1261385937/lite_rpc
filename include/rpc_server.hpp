#pragma once
#include <thread>
#include <memory>
#include <atomic>
#include <deque>
#include <mutex>
#include "rpc_server_session.hpp"

namespace lite_rpc {
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

		//optimize serialize and compress
		template<typename BodyType>
		void publish(std::string topic, std::string tag, BodyType&& body, bool check_verify = false) {
			// do not get conn_set, then publish. Or maybe get invalid sess.
			// do this under sub_conn_mtx_
			std::unique_lock<std::mutex> lock(sub_conn_mtx_);
			auto iter0 = sub_conn_.find(std::hash<std::string>()(topic));
			if (iter0 == sub_conn_.end()) { //none
				return;
			}
			auto& tag_conn = iter0->second;
			auto dispatch = [&tag_conn, check_verify](auto&&...args) {
				auto&& [_1, tag, _2, _3] = std::forward_as_tuple(std::forward<decltype(args)>(args)...);
				auto tag_iter = tag_conn.find(std::hash<std::string>()(tag));
				if (tag_iter == tag_conn.end()) {
					return;
				}

				auto conn_count = tag_iter->second.size();
				auto pa = session<Resource>::template make_packet<request_type::sub_pub>(std::forward<decltype(args)>(args)...);
				pa.need_free = false;
				auto iter = tag_iter->second.begin();

				if (!check_verify) {
					for (size_t i = 0; i < conn_count - 1; i++) {
						(*iter)->publish(pa);
						++iter;
					}
					//deal the last conn
					pa.need_free = true;
					(*iter)->publish(std::move(pa));
					return;
				}

				for (size_t i = 0; i < conn_count - 1; i++) {
					if ((*iter)->publish_enable_status()) {
						(*iter)->publish(pa);
					}
					++iter;
				}
				//deal the last conn
				bool can_release = false; //has no vertify conn, free the ext_buf if not nullptr
				pa.need_free = true;
				if ((*iter)->publish_enable_status()) {
					(*iter)->publish(std::move(pa));
					can_release = true;
				}
				if (!can_release && pa.ext_buf != nullptr) {
					::free(pa.ext_buf);
				}
			};

			dispatch(topic, std::string("*"), body, 0);
			dispatch(std::move(topic), std::move(tag), std::forward<BodyType>(body), 0);
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