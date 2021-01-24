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

using example_tuple_req = std::tuple<int, std::string, std::string>; //it is better, because of no MSGPACK_DEFINE
using example_tuple_res = std::string;

using get_server_msg_res = std::string;

using async_response_req = int;
using async_response_res = int;

using client_sync_call_req = double;
using client_sync_call_res = double;

using resource_req = std::string;
using resource_res = std::string;

using sess_res = std::string;

using rc_res = std::string;

using rc_req_req = int;
using rc_req_res = std::string;

using sess_rc_req_req = double;
using sess_rc_req_res = std::string;;