#include <future>
#include "rpc_client.hpp"
#include "rpc_define.h"

int main() {
	std::promise<void> f;
	auto c = std::make_shared<lite_rpc::rpc_client>();
	c->connect_async("127.0.0.1", "31236", 5, [&f]() {
		f.set_value();
	});
	f.get_future().get();

	example_struct_req struct_req{};
	struct_req.a = 11;
	struct_req.b = "22";
	strcpy_s(struct_req.c, "33");
	c->remote_call_async("example_struct", struct_req, [](example_struct_res&& res) {
		printf("res:%s\n", res.c_str()); //11+22+33
	});

	c->remote_call_async("example_tuple", example_tuple_req{ 11,"22","33" }, [](example_tuple_res&& res) {
		printf("res:%s\n", res.c_str()); //11+22+33
	});

	c->subscribe("haha", [](example_struct&& ex) {
		printf("subscribe:%s\n", (std::to_string(ex.a) + "+" + ex.b + "+" + ex.c).c_str());
	});

	getchar();
	return 0;
}