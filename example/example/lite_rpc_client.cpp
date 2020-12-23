//template<typename F, std::size_t ... Index>
//static constexpr void for_each_tuple(F&& f, std::index_sequence<Index...>) {
//	(std::forward<F>(f)(std::integral_constant<std::size_t, Index>()), ...);
//}
//
//template<typename ...Args>
//void default_value(Args&& ...args) {
//	auto tup = std::forward_as_tuple(std::forward<Args>(args)...);
//	int x = 11;
//	float y = 1.1f;
//	std::string z = "11";
//	uint64_t m = 22;
//	uint32_t n = 33;
//
//
//	for_each_tuple([&tup, &x, &y, &z, &m, &n](auto idx) {
//		using T = std::remove_reference_t<std::remove_cv_t<decltype(std::get<idx>(tup))>>;
//		if constexpr (std::is_same_v<T, int>) {
//			x = std::get<idx>(tup);
//		}
//		else if constexpr (std::is_same_v<T, float>) {
//			y = std::get<idx>(tup);
//		}
//		else if constexpr (std::is_same_v<T, std::string>) {
//			z = std::get<idx>(tup);
//		}
//		else if constexpr (std::is_same_v<T, uint64_t>) {
//			m = std::get<idx>(tup);
//		}
//		else if constexpr (std::is_same_v<T, uint32_t>) {
//			n = std::get<idx>(tup);
//		}
//	}, std::make_index_sequence<std::tuple_size_v<decltype(tup)>>());
//
//}


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

	c->subscribe("haha", [](example_struct&& ex) {
		printf("subscribe:%s\n", (std::to_string(ex.a) + "+" + ex.b + "+" + ex.c).c_str());
	});

	example_struct_req struct_req{};
	struct_req.a = 11;
	struct_req.b = "22";
	strcpy_s(struct_req.c, "33");
	c->remote_call_async("example_struct", struct_req, [](example_struct_res&& res) {
		printf("res:%s\n", res.c_str()); //11+22+33
	});

	//use tuple instead of struct is better.
	c->remote_call_async("example_tuple", example_tuple_req{ 11,"22","33" }, [](example_tuple_res&& res) {
		printf("res:%s\n", res.c_str()); //11+22+33
	});

	//no res from server
	c->remote_call_async("printf", example_tuple_req{ 11,"22","33" }, nullptr);

	//call class member func, no req to server
	c->remote_call_async("get_server_msg", "", [](get_server_msg_res&& res) {
		printf("server_ms is %s\n", res.c_str());
	});

	//call method <async_response>, the method is async in server endpoint.
	c->remote_call_async("async_response", async_response_req{ 1 }, [](async_response_res&& res) {
		printf("async_response result is %d\n", res);

	});


	//sync call is so easy with std::promise
	std::promise<client_sync_call_res> f_res;
	c->remote_call_async("client_sync_call", client_sync_call_req{ 100.3 }, [&f_res](client_sync_call_res&& res) {
		f_res.set_value(res);
	});
	auto r = f_res.get_future().get(); //get the result use sync

	getchar();
	return 0;
}