# lite_rpc
A lite rpc library with no IDL, no cross-language. Supports:
</br>req-res mode
</br>sub-pub mode
</br>automatically compress (zstd)
</br> 
</br>It is written by c++17.
</br>Strongly limit the rpc func param count for avoiding mistake. If you want more func param count, use std::tuple or struct.

## Dependence
Boost.asio. Because of asio coroutine (prepare for c++20 coroutine).
</br>MessagePack c++. Because of serialization, https://github.com/msgpack/msgpack-c/tree/cpp_master.
</br>Facebook zstd. Because of compression, https://github.com/facebook/zstd, enable or disable by macro in rpc_common.hpp.

## Compile
The lib do not need to compile, headonly.
</br>MessagePack c++, do not need to compile, headonly.
</br>Boost.asio, need to compile.
</br>Facebook zstd, need to compile if you enable compression.
</br>Boost.asio is not under the contrib dir, you should get it by yourself. MessagePack c++ and zstd is under the contrib dir.

## Usage
A very simple example:
</br>a common header file which is used for defining common data structure. Recommend this very strongly. It will make no error espacially for weak type rpc.
</br>For example: rpc_define.h
```c++
#pragma once
#include "rpc_common.hpp"

struct example_struct{
	int a;
	std::string b;
	char c[16];
	MSGPACK_DEFINE(a, b, c);
};


using example_struct_req = example_struct;
using example_struct_res = std::string;

using example_tuple_req = std::tuple<int, std::string, std::string>; //Recommend this, do not need MSGPACK_DEFINE.
using example_tuple_res = example_struct;
```
</br>client code:
```c++
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

	example_tuple_req tuple_req{ 11,"22","33" };
	c->remote_call_async("example_tuple", tuple_req, [](example_tuple_res&& res) {
		auto r = std::to_string(res.a) + "+" + std::move(res.b) + "+" + std::move(res.c);
		printf("res:%s\n",r.c_str()); //11+22+33
	});

	getchar();
	return 0;
}
```
</br>server code:
```c++
#include "rpc_server.hpp"
#include "rpc_define.h"

int main() {
	auto parallel_num = std::thread::hardware_concurrency();
	auto rpc_server = std::make_shared<lite_rpc::rpc_server<lite_rpc::empty_resource>>((uint16_t)31236);

	rpc_server->register_method("example_struct", [](example_struct_req&& req) {
		return example_struct_res{ std::to_string(req.a) + "+" + req.b + "+" + req.c };
	});

	rpc_server->register_method("example_tuple", [](std::weak_ptr<lite_rpc::session<lite_rpc::empty_resource>> sess, example_tuple_req&& req) {
		auto msg_id = sess.lock()->get_msg_id();
		std::thread th([sess, msg_id, request = std::move(req)]() mutable{
			std::this_thread::sleep_for(std::chrono::milliseconds(5000)); //simulate a long task
			auto conn = sess.lock();
			if (conn) {
				auto&& [a, b, c] = std::move(request);
				example_tuple_res res{};
				res.a = a;
				res.b = std::move(b);
				strcpy_s(res.c, c.data());
				conn->respond(example_tuple_res{ std::move(res) }, msg_id);
			}
		});
		th.detach();
	});

	rpc_server->run(parallel_num);
	return 0;
}
```
Under the example dir, has very detailed examples.
