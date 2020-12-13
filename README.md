# lite_rpc
A lite rpc library with no IDL, no cross-language. Supports:
</br>req-res mode
</br>sub-pub mode
</br>automatically compress (zstd)
</br>
</br>It is written by c++17.

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
</br>client:
```c++
#include <future>
#include "rpc_client.hpp"

int main() {
	std::promise<void> f;
	auto c = std::make_shared<simple_rpc::rpc_client>();
	c->connect_async("10.10.0.96", "31236", 5, [&f]() {
		f.set_value();
	});
	
	std::vector<std::shared_ptr<simple_rpc::rpc_client>> client;
	c->remote_call_async("echo_echo_echo_echo", "", [](std::string&& res) {
		printf("res:%s\n", res.c_str());
	});
	getchar();
	return 0;
}
```
</br>client printf   this is echo_echo_echo_echo res
</br>
</br>server:
```c++
#include "rpc_server.hpp"
int main() {
	auto parallel_num = std::thread::hardware_concurrency();
	auto rpc_server = std::make_shared<simple_rpc::rpc_server<simple_rpc::empty_resource>>(31236);
	rpc_server->register_method("echo_echo_echo_echo", []() {
		return std::string("this is echo_echo_echo_echo res");
	});
	rpc_server->run(parallel_num);
	return 0;
}
```
Under the example dir, has very detailed examples.
