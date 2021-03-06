//------------------------------------------------------------------------------
//  KjRedisSubscriberWorkQueue.cpp
//  (C) 2016 n.lee
//------------------------------------------------------------------------------
#include "KjRedisSubscriberWorkQueue.hpp"

#include <future>
#include "../RedisRootContextDef.hpp"
#include "../RedisSubscriber.h"

#include "KjRedisSubscriberConn.hpp"

struct redis_subscriber_thread_env_t {
	svrcore_pipeworker_t           *worker;

	kj::Own<kj::TaskSet>            tasks;
	kj::Own<KjRedisSubscriberConn>  conn;
};
static thread_local redis_subscriber_thread_env_t *stl_env = nullptr;

static kj::Promise<void>
check_quit_loop(CKjRedisSubscriberWorkQueue& q, redis_subscriber_thread_env_t& env, kj::PromiseFulfiller<void> *fulfiller) {
	if (!q.IsDone()) {
		// "delay and check quit loop" -- wait 500 ms
		return env.worker->endpointContext->GetTimer().afterDelay(500 * kj::MICROSECONDS)
			.then([&q, &env, fulfiller]() {
			// loop
			return check_quit_loop(q, env, fulfiller);
		});
	}

	//
	fulfiller->fulfill();
	return kj::READY_NOW;
}

static kj::Promise<void>
read_pipe_loop(CKjRedisSubscriberWorkQueue& q, redis_subscriber_thread_env_t& env, kj::AsyncIoStream& stream) {

	if (!q.IsDone()) {
		return stream.tryRead(&q._opCodeRecvBuf, 1, 1024)
			.then([&q, &env, &stream](size_t amount) {
			//
			// Get next work item.
			//
			int nCount = 0;
			while (q.Callbacks().try_dequeue(q._opCmd)) {
				env.conn->Send(q._opCmd);
				++nCount;
			}

			if (nCount > 0) {
				env.conn->Commit();
			}
			return read_pipe_loop(q, env, stream);
		});
	}
	return kj::READY_NOW;
}

//------------------------------------------------------------------------------
/**

*/
CKjRedisSubscriberWorkQueue::CKjRedisSubscriberWorkQueue(
	CRedisSubscriber *pRedisHandle,
	redis_stub_param_t& param,
	const std::function<void(std::string&, std::string&)>& workCb1,
	const std::function<void(std::string&, std::string&, std::string&)>& workCb2)
	: _refRedisHandle(pRedisHandle)
	, _refParam(param)
	, _refWorkCb1(workCb1)
	, _refWorkCb2(workCb2)
	, _callbacks(256) {

}

//------------------------------------------------------------------------------
/**

*/
CKjRedisSubscriberWorkQueue::~CKjRedisSubscriberWorkQueue() {
	
}

//------------------------------------------------------------------------------
/**

*/
void
CKjRedisSubscriberWorkQueue::Run(svrcore_pipeworker_t *worker) {
	//
	stl_env = new redis_subscriber_thread_env_t;
	stl_env->worker = worker;
	stl_env->tasks = redis_get_servercore()->NewTaskSet(*this);
	stl_env->conn = kj::heap<KjRedisSubscriberConn>(kj::addRef(*worker->endpointContext), _refParam, _refWorkCb1, _refWorkCb2);

	//
	InitTasks();

	// thread dispose
	stl_env->conn->Close();
	stl_env->conn = nullptr;
	stl_env->tasks = nullptr;

	delete stl_env;
	stl_env = nullptr;

	_finished = true;
}

//------------------------------------------------------------------------------
/**

*/
bool
CKjRedisSubscriberWorkQueue::Add(redis_cmd_pipepline_t&& cmd) {

	if (_done) {
		// error
		fprintf(stderr, "[CKjRedisSubscriberWorkQueue::Add()] can't enqueue, callback is dropped!!!");
		return false;
	}

	//
	// Add work item.
	//
	if (!_callbacks.enqueue(std::move(cmd))) {
		// error
		fprintf(stderr, "[CKjRedisSubscriberWorkQueue::Add()] enqueue failed, callback is dropped!!!");
		return false;
	}

	// write opcode to trunk pipe
	++_opCodeSend;
	redis_get_servercore()->PipeNotify(*_refRedisHandle->_refPipeWorker->pipeThread.pipe.get(), _opCodeSend);
	return true;
}

//------------------------------------------------------------------------------
/**

*/
void
CKjRedisSubscriberWorkQueue::Finish() {
	//
	// Set done flag and notify.
	//
	_done = true;

	// wait until finished
	while (!_finished) {
		util_sleep(10);
	}
}

//------------------------------------------------------------------------------
/**

*/
void
CKjRedisSubscriberWorkQueue::InitTasks() {

	auto paf = kj::newPromiseAndFulfiller<void>();
	stl_env->conn->Open();

	// "check_quit_loop"
	stl_env->tasks->add(
		check_quit_loop(
			*this,
			*stl_env,
			paf.fulfiller.get()));

	// "read_pipe_loop"
	stl_env->tasks->add(
		read_pipe_loop(
			*this,
			*stl_env,
			stl_env->worker->endpointContext->GetEndpoint()));

	//
	paf.promise.wait(stl_env->worker->endpointContext->GetWaitScope());
}

//------------------------------------------------------------------------------
/**

*/
void
CKjRedisSubscriberWorkQueue::taskFailed(kj::Exception&& exception) {
	// fatal
	StdLog *pLog = redis_get_log();
	if (pLog)
		pLog->logprint(LOG_LEVEL_FATAL, "\n\n\n!!![CKjRedisSubscriberWorkQueue::taskFailed()] exception_desc(%s)!!!\n\n\n",
			exception.getDescription().cStr());

	fprintf(stderr, "\n[CKjRedisSubscriberWorkQueue::taskFailed()] desc(%s) -- pause!!!\n",
		exception.getDescription().cStr());

	kj::throwFatalException(kj::mv(exception));
}

/** -- EOF -- **/