
#include "rpc_server.hpp"
#include "rpc_define.h"

void printf_req(example_tuple_req&& req) {
	auto&& [a, b, c] = std::move(req);
	printf("req is:%d %s %s\n", a, b.c_str(), c.c_str());
}

class server {
public:
	auto get_server_msg() {
		return get_server_msg_res{ R"(
{"adsorbMinPriceValid":1,"adsorbPixel":15,"adsorbValid":1,"againstOpening":0,"bothEnds":0,
"breakthroughOpening":1,"breakthroughRegressionOpening":0,"constantSpeedMove":0,"grabPixel":8,"hGridMinPriceMultiple":1,
"immediateOpening":0,"klineTodayShow":0,"klineTodayShowType":0,"leftright":1,"longScale":0,"opencloseshow":0,"openingClosing":0,
"overPriceClosing":0,"pressureSupportScope":0,"realbottom":20,"realtime":60,"realtop":20,"regressionOpening":0,"rulerAlpha":36,
"rulerLongLineLen":90,"rulerReverse":0,"rulerShortLineLen":20,"rulerSingleDoubleScale":0,"rulerStepCnt":15,"rulerStepMinPixel":0,
"rulerTextValid":0,"rulerTitleValid":0,"rulerValid":1,"shapeJumpTimeout":0.3,"shapeJumpTimeout2":0.2,"smart":0,"stockbottom":300,
"stockright":200,"stocktime":60,"stocktop":300,"tradebubble":0,"triggerClosing":0,"triggerOpening":0,"twinkleShow":1,"twinkleValueDn":2,
"twinkleValueUp":2"rulerLongLineLen":90,"rulerReverse":0,"rulerShortLineLen":20,"rulerSingleDoubleScale":0,"rulerStepCnt":15,"rulerStepMinPixel":0,
"rulerTextValid":0,"rulerTitleValid":0,"rulerValid":1,"shapeJumpTimeout":0.3,"shapeJumpTimeout2":0.2,"smart":0,"stockbottom":300,
"stockright":200,"stocktime":60,"stocktop":300,"tradebubble":0,"triggerClosing":0,"triggerOpening":0,"twinkleShow":1,"twinkleValueDn":2,
"twinkleValueUp":2)" };
	}
};


struct common_resource {
	std::string connection_pool = "connection_pool";
	std::string thread_pool = "thread_pool";
};

int main() {
	auto parallel_num = std::thread::hardware_concurrency();
	//auto rpc_server = std::make_shared<lite_rpc::rpc_server<lite_rpc::empty_resource>>((uint16_t)31236);

	std::vector<common_resource> res(parallel_num);
	auto rpc_server = std::make_shared<lite_rpc::rpc_server<common_resource>>((uint16_t)31236, res);


	//pubish every 3s
	std::thread th([rpc_server]() {
		while (true) {
			std::this_thread::sleep_for(std::chrono::milliseconds(3000));
			example_struct ex_aa{};
			ex_aa.a = 11;
			ex_aa.b = "22";
			strcpy_s(ex_aa.c, "aa");
			rpc_server->publish("haha", "aa", ex_aa);

			example_struct ex_ee{};
			ex_ee.a = 1111;
			ex_ee.b = "2222";
			strcpy_s(ex_ee.c, "ee");
			rpc_server->publish("haha", "ee", ex_ee);
			
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
	rpc_server->register_method("async_response", [](std::weak_ptr<lite_rpc::session<common_resource>> sess, async_response_req&& req) {
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

	rpc_server->register_method("resource", [](std::weak_ptr<lite_rpc::session<common_resource>> sess, const common_resource& rc) {
		//still in corotinue
		sess.lock()->coro_respond(resource_res{ rc.connection_pool + " " + rc.thread_pool });
	});

	rpc_server->register_method("sess", [](std::weak_ptr<lite_rpc::session<common_resource>> sess) {
		//still in corotinue
		sess.lock()->coro_respond(sess_res{ "this is test sess" });
	});

	rpc_server->register_method("rc", [](const common_resource& rc) {
		return rc_res{ "this is test rc " } + rc.connection_pool + " " + rc.thread_pool;
	});

	rpc_server->register_method("rc_req", [](const common_resource& rc, rc_req_req&& req) {
		return rc_res{ "this is test rc_req " } + rc.connection_pool + " " + rc.thread_pool + " " + std::to_string(req);
	});

	rpc_server->register_method("sess_rc_req", [](std::weak_ptr<lite_rpc::session<common_resource>> sess, const common_resource& rc, sess_rc_req_req&& req) {
		auto conn = sess.lock();
		conn->respond(sess_rc_req_res{ "this is test sess_rc_req " } + rc.connection_pool + " " + rc.thread_pool + " " + std::to_string(req), conn->get_msg_id());
	});

	rpc_server->run(parallel_num);
	return 0;
}
