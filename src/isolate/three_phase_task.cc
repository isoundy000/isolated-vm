#include "three_phase_task.h"
#include "../external_copy.h"

using namespace v8;
using std::unique_ptr;

namespace ivm {

/**
 * CalleeInfo implementation
 */
ThreePhaseTask::CalleeInfo::CalleeInfo(
	Local<Promise::Resolver> resolver,
	Local<Context> context,
	Local<StackTrace> stack_trace
) : remotes(resolver, context, stack_trace) {
	IsolateEnvironment* env = IsolateEnvironment::GetCurrent();
	if (env->IsDefault()) {
		async = node::EmitAsyncInit(env->GetIsolate(), resolver->GetPromise(), v8_symbol("isolated-vm"));
	}
}

ThreePhaseTask::CalleeInfo::~CalleeInfo() {
	IsolateEnvironment* env = IsolateEnvironment::GetCurrent();
	if (env->IsDefault()) {
		node::EmitAsyncDestroy(env->GetIsolate(), async);
	}
}

/**
 * Wrapper around node's version of the same class which does nothing if this isn't the node
 * isolate.
 *
 * nb: CallbackScope sets up a v8::TryCatch so if you need to catch an exception do this *before*
 * the v8::TryCatch.
 */
struct CallbackScope {
	unique_ptr<node::CallbackScope> scope;

	CallbackScope(node::async_context async, Local<Object> resource) {
		IsolateEnvironment* env = IsolateEnvironment::GetCurrent();
		if (env->IsDefault()) {
			scope = std::make_unique<node::CallbackScope>(env->GetIsolate(), resource, async);
		}
	}
};

/**
 * Phase2Runner implementation
 */
ThreePhaseTask::Phase2Runner::Phase2Runner(
	unique_ptr<ThreePhaseTask> self,
	unique_ptr<CalleeInfo> info
) :
	self(std::move(self)),
	info(std::move(info))	{}

ThreePhaseTask::Phase2Runner::~Phase2Runner() {
	if (!did_run) {
		// The task never got to run
		struct Phase3Orphan : public Runnable {
			unique_ptr<ThreePhaseTask> self;
			unique_ptr<CalleeInfo> info;

			Phase3Orphan(
				unique_ptr<ThreePhaseTask> self,
				unique_ptr<CalleeInfo> info
			) :
				self(std::move(self)),
				info(std::move(info)) {}

			void Run() final {
				// Revive our persistent handles
				Isolate* isolate = Isolate::GetCurrent();
				auto context_local = info->remotes.Deref<1>();
				Context::Scope context_scope(context_local);
				auto promise_local = info->remotes.Deref<0>();
				CallbackScope callback_scope(info->async, promise_local);
				// Throw from promise
				Local<Object> error = Exception::Error(v8_string("Isolate is disposed")).As<Object>();
				StackTraceHolder::AttachStack(error, info->remotes.Deref<2>());
				Unmaybe(promise_local->Reject(context_local, error));
				isolate->RunMicrotasks();
			}
		};
		// Schedule a throw task back in first isolate
		auto holder = info->remotes.GetIsolateHolder();
		holder->ScheduleTask(
			std::make_unique<Phase3Orphan>(
				std::move(self),
				std::move(info)
			), false, true
		);
	}
}

void ThreePhaseTask::Phase2Runner::Run() {

	// This class will be used if Phase2() throws an error
	struct Phase3Failure : public Runnable {
		unique_ptr<ThreePhaseTask> self;
		unique_ptr<CalleeInfo> info;
		unique_ptr<ExternalCopy> error;

		Phase3Failure(
			unique_ptr<ThreePhaseTask> self,
			unique_ptr<CalleeInfo> info,
			unique_ptr<ExternalCopy> error
		) :
			self(std::move(self)),
			info(std::move(info)),
			error(std::move(error)) {}

		void Run() final {
			// Revive our persistent handles
			Isolate* isolate = Isolate::GetCurrent();
			auto context_local = info->remotes.Deref<1>();
			Context::Scope context_scope(context_local);
			auto promise_local = info->remotes.Deref<0>();
			CallbackScope callback_scope(info->async, promise_local);
			Local<Value> rejection;
			if (error) {
				rejection = error->CopyInto();
			} else {
				rejection = Exception::Error(v8_string("An exception was thrown. Sorry I don't know more."));
			}
			if (rejection->IsObject()) {
				StackTraceHolder::ChainStack(rejection.As<Object>(), info->remotes.Deref<2>());
			}
			// If Reject fails then I think that's bad..
			Unmaybe(promise_local->Reject(context_local, rejection));
			isolate->RunMicrotasks();
		}
	};

	// This is called if Phase2() does not throw
	struct Phase3Success : public Runnable {
		unique_ptr<ThreePhaseTask> self;
		unique_ptr<CalleeInfo> info;

		Phase3Success(
			unique_ptr<ThreePhaseTask> self,
			unique_ptr<CalleeInfo> info
		) :
			self(std::move(self)),
			info(std::move(info)) {}

		void Run() final {
			Isolate* isolate = Isolate::GetCurrent();
			auto context_local = info->remotes.Deref<1>();
			Context::Scope context_scope(context_local);
			auto promise_local = info->remotes.Deref<0>();
			CallbackScope callback_scope(info->async, promise_local);
			FunctorRunners::RunCatchValue([&]() {
				// Final callback
				Unmaybe(promise_local->Resolve(context_local, self->Phase3()));
			}, [&](Local<Value> error) {
				// Error was thrown
				if (error->IsObject()) {
					StackTraceHolder::AttachStack(error.As<Object>(), info->remotes.Deref<2>());
				}
				Unmaybe(promise_local->Reject(context_local, error));
			});
			isolate->RunMicrotasks();
		}
	};

	did_run = true;
	FunctorRunners::RunCatchExternal(IsolateEnvironment::GetCurrent()->DefaultContext(), [ this ]() {
		// Continue the task
		self->Phase2();
		IsolateEnvironment::GetCurrent()->TaskEpilogue();
		auto holder = info->remotes.GetIsolateHolder();
		holder->ScheduleTask(std::make_unique<Phase3Success>(std::move(self), std::move(info)), false, true);
	}, [ this ](unique_ptr<ExternalCopy> error) {

		// Schedule a task to enter the first isolate so we can throw the error at the promise
		auto holder = info->remotes.GetIsolateHolder();
		holder->ScheduleTask(std::make_unique<Phase3Failure>(std::move(self), std::move(info), std::move(error)), false, true);
	});
}

/**
 * Phase2RunnerIgnored implementation
 */
ThreePhaseTask::Phase2RunnerIgnored::Phase2RunnerIgnored(unique_ptr<ThreePhaseTask> self) : self(std::move(self)) {}

void ThreePhaseTask::Phase2RunnerIgnored::Run() {
	TryCatch try_catch(Isolate::GetCurrent());
	try {
		self->Phase2();
		IsolateEnvironment::GetCurrent()->TaskEpilogue();
	} catch (const js_runtime_error& cc_error) {}
}

/**
 * RunSync implementation
 */
Local<Value> ThreePhaseTask::RunSync(IsolateHolder& second_isolate) {
	// Grab a reference to second isolate
	auto second_isolate_ref = second_isolate.GetIsolate();
	if (!second_isolate_ref) {
		throw js_generic_error("Isolated is disposed");
	}
	if (second_isolate_ref->GetIsolate() == Isolate::GetCurrent()) {
		// Shortcut when calling a sync method belonging to the currently entered isolate. This avoids
		// the deadlock protection below
		Phase2();
	} else {

		// Deadlock protection
		if (!IsolateEnvironment::ExecutorLock::IsDefaultThread()) {
			throw js_generic_error(
				"Calling a synchronous isolated-vm function from within an asynchronous isolated-vm function is not allowed."
			);
		}

		// Run Phase2 and externalize errors
		unique_ptr<ExternalCopy> error;
		bool is_recursive = Locker::IsLocked(second_isolate_ref->GetIsolate());
		{
			IsolateEnvironment::ExecutorLock lock(*second_isolate_ref);
			FunctorRunners::RunCatchExternal(second_isolate_ref->DefaultContext(), [ this, is_recursive, &second_isolate_ref ]() {
				Phase2();
				if (!is_recursive) {
					second_isolate_ref->TaskEpilogue();
				}
			}, [ &error ](unique_ptr<ExternalCopy> error_inner) {

				// We need to stash the error in the outer unique_ptr because the executor lock is still up
				error = std::move(error_inner);
			});
		}

		if (error) {
			// Throw to outer isolate
			Isolate* isolate = Isolate::GetCurrent();
			Local<Value> error_copy = error->CopyInto();
			if (error_copy->IsObject()) {
				StackTraceHolder::ChainStack(error_copy.As<Object>(), StackTrace::CurrentStackTrace(isolate, 10));
			}
			isolate->ThrowException(error_copy);
			throw js_runtime_error();
		}
	}
	// Final phase
	return Phase3();
}

} // namespace ivm
