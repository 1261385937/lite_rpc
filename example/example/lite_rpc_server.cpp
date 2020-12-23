
#include "rpc_server.hpp"
#include "rpc_define.h"

void printf_req(example_tuple_req&& req) {
	auto&& [a, b, c] = std::move(req);
	printf("req is:%d %s %s\n", a, b.c_str(), c.c_str());
}

class server {
public:
	auto get_server_msg() {
		return get_server_msg_res{ "this is server abouit test" };
	}
};


int main() {
	auto parallel_num = std::thread::hardware_concurrency();
	auto rpc_server = std::make_shared<lite_rpc::rpc_server<lite_rpc::empty_resource>>((uint16_t)31236);

	std::thread th([rpc_server]() {
		example_struct ex{};
		ex.a = 11;
		ex.b = "22";
		strcpy_s(ex.c, "33");
		while (true) {
			rpc_server->publish("haha", ex);
			std::this_thread::sleep_for(std::chrono::milliseconds(3000));
		}
	});
	th.detach();

	rpc_server->register_method("example_struct", [](example_struct_req&& req) {
		return example_struct_res{ std::to_string(req.a) + "+" + req.b + "+" + req.c };
	});

	rpc_server->register_method("example_tuple", [](example_tuple_req&& req) {
		auto&& [a, b, c] = std::move(req);
		return example_tuple_res{ std::to_string(a) + "+" + std::move(b) + "+" + std::move(c) };
	});

	rpc_server->register_method("printf", printf_req);

	server s;
	rpc_server->register_method("get_server_msg", &server::get_server_msg, &s);

	//simulate a async response
	rpc_server->register_method("async_response", [](std::weak_ptr<lite_rpc::session<lite_rpc::empty_resource>> sess, async_response_req&& req) {
		auto id = sess.lock()->get_msg_id();
		std::thread th([request = std::move(req), sess, id]() {
			std::this_thread::sleep_for(std::chrono::milliseconds(3000));
			auto r = request + 10000;
			auto conn = sess.lock();
			if (conn) { //the conn maybe lost during the 3000ms
				conn->respond(async_response_res{ r }, id);
			}
		});
		th.detach();
	});

	rpc_server->register_method("client_sync_call", [](client_sync_call_req&& req) {
		return client_sync_call_res{ req };
	});

	rpc_server->run(parallel_num);
	return 0;
}
