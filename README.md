# lite_rpc
A lite rpc library with no IDL, no cross-language. Supports:
</br>req-res mode
</br>sub-pub mode
</br>automatically compress (zstd)
</br> 
## Note
It is written by c++17.
</br>Strongly limit the rpc func param number for avoiding mistake. If you want more func param number, please use std::tuple or struct package them.
</br>rpc_server(typename Resource) is used for avoiding competition. If you make a special connection_pool for every parallel, you will not get competition, and the special connection_pool just need one item. Usually the parallel number equal to hardware_concurrency.
	
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
</br>a common header file which is used for defining common data structure. Recommend this very strongly. It will make no error espacially for weak type rpc.
</br>
</br>rpc_define.h 
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
</br>A very simple example about request-response: 
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

	rpc_server->run(parallel_num);
	return 0;
}
```

</br></br>A very simple example about subscribe-publish: 
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


	c->subscribe("haha", [](example_struct&& ex) {
		printf("subscribe:%s\n", (std::to_string(ex.a) + "+" + ex.b + "+" + ex.c).c_str());
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

	rpc_server->run(parallel_num);
	return 0;
}
```
</br>Under the example dir, has very detailed examples. 
