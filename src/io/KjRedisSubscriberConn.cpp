//------------------------------------------------------------------------------
//  KjRedisSubscriberConn.cpp
//  (C) 2016 n.lee
//------------------------------------------------------------------------------
#include "KjRedisSubscriberConn.hpp"

#pragma push_macro("ERROR")
#undef ERROR
#pragma push_macro("VOID")
#undef VOID

#include <kj/debug.h>

#pragma pop_macro("ERROR")
#pragma pop_macro("VOID")

#include <time.h>

#include "base/RedisError.h"
#include "RedisService.h"

static void
write_redis_connection_crash_error(const char *err_txt) {
	char log_file_name[256];
	char time_suffix[32];

	time_t t = time(nullptr);

#if defined(__CYGWIN__) || defined( _WIN32)
	struct tm tp;
	localtime_s(&tp, &t);
	_snprintf_s(
		time_suffix
		, sizeof(time_suffix)
		, "_%d-%02d-%02d_%02d_%02d_%02d"
		, tp.tm_year + 1900
		, tp.tm_mon + 1
		, tp.tm_mday
		, tp.tm_hour
		, tp.tm_min
		, tp.tm_sec);
#else
	struct tm tp;
	localtime_r(&t, &tp);
	snprintf(
		time_suffix
		, sizeof(time_suffix)
		, "_%d-%02d-%02d_%02d_%02d_%02d"
		, tp.tm_year + 1900
		, tp.tm_mon + 1
		, tp.tm_mday
		, tp.tm_hour
		, tp.tm_min
		, tp.tm_sec);
#endif

#if defined(__CYGWIN__) || defined( _WIN32)
	_snprintf_s(log_file_name, sizeof(log_file_name), "syslog/redis_connection_crash_%s.error",
		time_suffix);
#else
	snprintf(log_file_name, sizeof(log_file_name), "syslog/redis_connection_crash_%s.error",
		time_suffix);
#endif

	FILE *ferr = fopen(log_file_name, "at+");
	fprintf(ferr, "redis connection crashed:\n%s\n",
		err_txt);
	fclose(ferr);
}

//------------------------------------------------------------------------------
/**

*/
KjRedisSubscriberConn::KjRedisSubscriberConn(
	kj::Own<KjSimpleThreadIoContext> tioContext,
	redis_stub_param_t& param,
	const std::function<void(std::string&, std::string&)>& workCb1,
	const std::function<void(std::string&, std::string&, std::string&)>& workCb2)
	: _tioContext(kj::mv(tioContext))
	, _tasks(_tioContext->CreateTaskSet(*this))
	, _refParam(param)
	, _refWorkCb1(workCb1)
	, _refWorkCb2(workCb2	)
	, _kjconn(kj::addRef(*_tioContext), (uintptr_t)(this)) {
	//
	Init();
}

//------------------------------------------------------------------------------
/**
*/
KjRedisSubscriberConn::~KjRedisSubscriberConn() {
	Disconnect();
}

//------------------------------------------------------------------------------
/**

*/
void
KjRedisSubscriberConn::OnClientConnect(KjTcpConnection&, uint64_t connid) {
	// start read
	auto p1 = _kjconn.StartReadOp(
		std::bind(&KjRedisSubscriberConn::OnClientReceive,
			this,
			std::placeholders::_1,
			std::placeholders::_2)
	);
	_tasks->add(kj::mv(p1), "kjconn start read op");

	// connection init
	{
		std::deque<CKjRedisSubscriberWorkQueue::cmd_pipepline_t> dqTmp;
		dqTmp.insert(dqTmp.end(), _dqInit.begin(), _dqInit.end());

		for (auto& cp : _dqCommon) {
			if (cp._sn > 0
				&& cp._state < CKjRedisSubscriberWorkQueue::cmd_pipepline_t::CMD_PIPELINE_STATE_PROCESS_OVER) {
				// it is common cmd pipeline and not process over yest
				dqTmp.push_back(std::move(cp));
			}
		}
		_dqCommon.swap(dqTmp);

		for (auto& cp : _dqCommon) {
			// resending
			cp._state = CKjRedisSubscriberWorkQueue::cmd_pipepline_t::CMD_PIPELINE_STATE_SENDING;
		}

		// recommit
		Commit();
	}
}

//------------------------------------------------------------------------------
/**

*/
void
KjRedisSubscriberConn::OnClientDisconnect(KjTcpConnection&, uint64_t connid) {

}

//------------------------------------------------------------------------------
/**

*/
void
KjRedisSubscriberConn::OnClientReceive(KjTcpConnection&, bip_buf_t& bbuf) {

	try {
		_builder.ProcessInput(bbuf);
	}
	catch (const CRedisError&) {
		return;
	}

	while (_builder.IsReplyAvailable()) {

		auto& reply = _builder.GetFront();
		if (reply.is_error()) {

			std::string sDesc = "[KjRedisSubscriberConn::OnClientReceive()] !!! reply error !!! ";
			sDesc += reply.as_string();
			fprintf(stderr, "\n\n\n%s\n", sDesc.c_str());

			//
			_builder.PopFront();
			throw CRedisError(sDesc.c_str());
		}
	
		bool bIsMessage = false;
		if (reply.ok()
			&& reply.is_array()) {
			std::vector<CRedisReply> v = std::move(reply.as_array());
			if (v.size() >= 4
				&& v[0].is_string()
				&& v[1].is_string()
				&& v[2].is_string()
				&& v[3].is_string()) {
				//
				std::string sMessageType = std::move(v[0].as_string());
				std::string sPattern = std::move(v[1].as_string());
				std::string sChannel = std::move(v[2].as_string());
				std::string sMessage = std::move(v[3].as_string());
			
				//
				bIsMessage = (sMessageType == "pmessage");
				if (bIsMessage) {
					_refWorkCb2(std::move(sPattern), std::move(sChannel), std::move(sMessage));
				}
			}
			else if (v.size() >= 3
				&& v[0].is_string()
				&& v[1].is_string()
				&& v[2].is_string()) {
				//
				std::string sMessageType = std::move(v[0].as_string());
				std::string sChannel = std::move(v[1].as_string());
				std::string sMessage = std::move(v[2].as_string());

				//
				bIsMessage = (sMessageType == "message");
				if (bIsMessage) {
					_refWorkCb1(std::move(sChannel), std::move(sMessage));
				}
			}
		}

		if (!bIsMessage) {
			auto& cp = _dqCommon.front();

			// check committing num
			if (_committing_num <= 0
				|| _dqCommon.size() <= 0) {

				std::string sDesc = "[KjRedisSubscriberConn::OnClientReceive()] !!! got reply but the cmd pipeline is already discarded !!! ";
				fprintf(stderr, "\n\n\n%s\n", sDesc.c_str());

				//
				_builder.PopFront();
				throw CRedisError(sDesc.c_str());
			}

			// check cmd pipeline state
			if (cp._state < CKjRedisSubscriberWorkQueue::cmd_pipepline_t::CMD_PIPELINE_STATE_COMMITTING) {

				std::string sDesc = "[KjRedisSubscriberConn::OnClientReceive()] !!! got reply but the cmd pipeline is in a corrupted state !!! ";
				fprintf(stderr, "\n\n\n%s\n", sDesc.c_str());

				//
				_builder.PopFront();
				throw CRedisError(sDesc.c_str());
			}

			// go on processing -- non-tail reply is skipped, just add process num
			++cp._processed_num;
			cp._state = CKjRedisSubscriberWorkQueue::cmd_pipepline_t::CMD_PIPELINE_STATE_PROCESSING;

			// process over -- it is tail reply(the last reply of the cmd pipeline), run callback on it
			if (cp._processed_num >= cp._built_num) {

				if (cp._reply_cb) cp._reply_cb(std::move(reply));
				if (cp._dispose_cb) cp._dispose_cb();

				cp._state = CKjRedisSubscriberWorkQueue::cmd_pipepline_t::CMD_PIPELINE_STATE_PROCESS_OVER;

				//
				_dqCommon.pop_front();
				--_committing_num;
			}
		}

		//
		_builder.PopFront();
	}
}

//------------------------------------------------------------------------------
/**

*/
void
KjRedisSubscriberConn::Open() {

	fprintf(stderr, "[KjRedisSubscriberConn::Open()] connect to ip(%s)port(%d)...\n",
		_refParam._ip.c_str(), _refParam._port);

	Connect(_refParam._ip, _refParam._port);
}

//------------------------------------------------------------------------------
/**

*/
void
KjRedisSubscriberConn::Close() {

}

//------------------------------------------------------------------------------
/**

*/
void
KjRedisSubscriberConn::Connect(const std::string& host, unsigned short port) {

	auto p1 = _kjconn.Connect(
		host,
		port,
		std::bind(&KjRedisSubscriberConn::OnClientConnect, this, std::placeholders::_1, std::placeholders::_2),
		std::bind(&KjRedisSubscriberConn::OnClientDisconnect, this, std::placeholders::_1, std::placeholders::_2)
	);
	_tasks->add(kj::mv(p1), "kjconn connect");
}

//------------------------------------------------------------------------------
/**

*/
void
KjRedisSubscriberConn::Disconnect() {
	_kjconn.Disconnect();
}

//------------------------------------------------------------------------------
/**

*/
void
KjRedisSubscriberConn::Init() {
	//
	std::string sAllCommands;
	int nBuiltNum = 0;

	// auth
	const std::string& sPassword = _refParam._sPassword;
	if (sPassword.length() > 0) {

		std::string sSingleCommand;
		CRedisService::Send({ "AUTH", sPassword }, sSingleCommand, sAllCommands, nBuiltNum);

		//
		auto cp = CKjRedisSubscriberWorkQueue::CreateCmdPipeline(
			0,
			sAllCommands,
			nBuiltNum,
			nullptr,
			nullptr);

		//
		_dqInit.push_back(std::move(cp));
		sAllCommands.resize(0);
		nBuiltNum = 0;
	}
}

//------------------------------------------------------------------------------
/**

*/
void
KjRedisSubscriberConn::AutoReconnect() {
	//
	_tasks->add(_kjconn.DelayReconnect(), "kjconn delay reconnect");

	// 
	for (auto& cp : _dqCommon) {
		cp._state = CKjRedisSubscriberWorkQueue::cmd_pipepline_t::CMD_PIPELINE_STATE_QUEUEING;
	}
	_committing_num = 0;
}

//------------------------------------------------------------------------------
/**

*/
kj::Promise<void>
KjRedisSubscriberConn::CommitLoop() {
	if (IsConnected()) {
		for (auto& cp : _dqCommon) {
			if (CKjRedisSubscriberWorkQueue::cmd_pipepline_t::CMD_PIPELINE_STATE_SENDING == cp._state) {

				_tasks->add(_kjconn.Write(cp._commands.c_str(), cp._commands.length()), "kjconn write commands");

				cp._state = CKjRedisSubscriberWorkQueue::cmd_pipepline_t::CMD_PIPELINE_STATE_COMMITTING;
				++_committing_num;
			}
		}

		// commit over
		return kj::READY_NOW;
	}
	else {
		auto p1 = _tioContext->AfterDelay(1 * kj::SECONDS, "delay and commit loop")
			.then([this]() {

			return CommitLoop();
		});
		return kj::mv(p1);
	}
}

//------------------------------------------------------------------------------
/**

*/
void
KjRedisSubscriberConn::taskFailed(kj::Exception&& exception) {
	char chDesc[1024];
#if defined(__CYGWIN__) || defined( _WIN32)
	_snprintf_s(chDesc, sizeof(chDesc), "[KjRedisSubscriberConn::taskFailed()] desc(%s) -- auto reconnect.\n",
		exception.getDescription().cStr());
#else
	snprintf(chDesc, sizeof(chDesc), "[KjRedisSubscriberConn::taskFailed()] desc(%s) -- auto reconnect.\n",
		exception.getDescription().cStr());
#endif
	fprintf(stderr, chDesc);
	write_redis_connection_crash_error(chDesc);

	// force disconnect
	Disconnect();

	//
	_builder.Reset();
	AutoReconnect();
}

/** -- EOF -- **/