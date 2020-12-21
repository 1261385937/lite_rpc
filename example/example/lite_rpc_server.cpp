#include "rpc_server.hpp"
#include "rpc_define.h"

int main() {
	auto parallel_num = std::thread::hardware_concurrency();
	auto rpc_server = std::make_shared<lite_rpc::rpc_server<lite_rpc::empty_resource>>((uint16_t)31236);

	std::thread th([rpc_server]() {
		example_struct ex{};
		ex.a = 11;
		ex.b = "22";
		strcpy_s(ex.c, "33");
		
		while (true)
		{
			rpc_server->publish("haha", ex);
			std::this_thread::sleep_for(std::chrono::milliseconds(3000));
		}
	});

	rpc_server->register_method("example_struct", [](example_struct_req&& req) {
		return example_struct_res{ std::to_string(req.a) + "+" + req.b + "+" + req.c };
	});

	rpc_server->register_method("example_tuple", [](example_tuple_req&& req) {
		auto&& [a, b, c] = std::move(req);
		return example_tuple_res{ std::to_string(a) + "+" + std::move(b) + "+" + std::move(c) };
	});

	rpc_server->run(parallel_num);
	return 0;
}
