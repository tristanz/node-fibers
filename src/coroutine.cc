#include "coroutine.h"
#include <assert.h>
#define __GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <ucontext.h>

#include <stdexcept>
#include <stack>
#include <vector>

// TODO: It's clear I'm missing something with respects to ResourceConstraints.set_stack_limit.
// No matter what I give it, it seems the stack size is always the same. And then if the actual
// amount of memory allocated for the stack is too small it seg faults. It seems 265k is as low as
// I can go without fixing the underlying bug.
#define STACK_SIZE (1024 * 64)
#define MAX_POOL_SIZE 120

#include <iostream>
using namespace std;

typedef void(*pthread_dtor_t)(void*);
static pthread_key_t thread_key;
static bool did_hook_v8 = false;

/**
 * LocalLocalThread is only used internally for this library. It keeps track of all the fibers this
 * thread is currently running, and handles all the fiber-local storage logic. We store a handle to
 * a LocalLocalThread object in TLS, and then it emulates TLS on top of fibers.
 */
class LocalThread {
	private:
		static vector<pthread_dtor_t> dtors;
		vector<Coroutine*> fiber_pool;

		LocalThread() : delete_me(NULL) {
			current_fiber = new Coroutine(*this);
		}

		~LocalThread() {
			for (size_t ii = 0; ii < fiber_pool.size(); ++ii) {
				delete fiber_pool[ii];
			}
		}

	public:
		volatile Coroutine* current_fiber;
		volatile Coroutine* delete_me;

		static void free(void* that) {
			delete static_cast<LocalThread*>(that);
		}

    static LocalThread& get() {
      LocalThread* thread = static_cast<LocalThread*>(pthread_getspecific(thread_key));
      if (thread == NULL) {
        thread = new LocalThread;
        pthread_setspecific(thread_key, thread);
      }
      return *thread;
		}

		void coroutine_fls_dtor(Coroutine& fiber) {
			bool did_delete;
			do {
				did_delete = false;
				for (size_t ii = 0; ii < fiber.fls_data.size(); ++ii) {
					if (fiber.fls_data[ii]) {
						if (dtors[ii]) {
							void* tmp = fiber.fls_data[ii];
							fiber.fls_data[ii] = NULL;
							dtors[ii](tmp);
							did_delete = true;
						} else {
							fiber.fls_data[ii] = NULL;
						}
					}
				}
			} while (did_delete);
		}

		void fiber_did_finish(Coroutine& fiber) {
			if (fiber_pool.size() < MAX_POOL_SIZE) {
				fiber_pool.push_back(&fiber);
			} else {
				coroutine_fls_dtor(fiber);
				// Can't delete right now because we're currently on this stack!
				assert(delete_me == NULL);
				delete_me = &fiber;
			}
		}

		Coroutine& create_fiber(Coroutine::entry_t& entry, void* arg) {
			if (!fiber_pool.empty()) {
				Coroutine& fiber = *fiber_pool.back();
				fiber_pool.pop_back();
				fiber.reset(entry, arg);
				return fiber;
			}

			return *new Coroutine(*this, entry, arg);
		}

		void* get_specific(pthread_key_t key) {
			if (const_cast<Coroutine*>(current_fiber)->fls_data.size() <= key) {
				return NULL;
			}
			return const_cast<Coroutine*>(current_fiber)->fls_data[key];
		}

		void set_specific(pthread_key_t key, const void* data) {
			if (const_cast<Coroutine*>(current_fiber)->fls_data.size() <= key) {
				const_cast<Coroutine*>(current_fiber)->fls_data.resize(key + 1);
			}
			const_cast<Coroutine*>(current_fiber)->fls_data[key] = (void*)data;
		}

		static pthread_key_t key_create(pthread_dtor_t dtor) {
			dtors.push_back(dtor);
			return dtors.size() - 1; // TODO: This is NOT thread-safe! =O
		}

		void key_delete(pthread_key_t key) {
			if (!dtors[key]) {
				return;
			}
			// This doesn't call the dtor on all threads / fibers. Do I really care?
			if (get_specific(key)) {
				dtors[key](get_specific(key));
				set_specific(key, NULL);
			}
		}
};
vector<pthread_dtor_t> LocalThread::dtors;

/**
 * Coroutine class definition
 */
void Coroutine::trampoline(Coroutine &that) {
	while (true) {
		that.entry(const_cast<void*>(that.arg));
	}
}

Coroutine& Coroutine::current() {
  return *const_cast<Coroutine*>(LocalThread::get().current_fiber);
}

const bool Coroutine::is_local_storage_enabled() {
	return did_hook_v8;
}

Coroutine::Coroutine(LocalThread& t) : thread(t) {}

Coroutine::Coroutine(LocalThread& t, entry_t& entry, void* arg) :
	thread(t),
	stack(STACK_SIZE),
	entry(entry),
	arg(arg) {
	getcontext(&context);
	context.uc_stack.ss_size = STACK_SIZE;
	context.uc_stack.ss_sp = &stack[0];
	makecontext(&context, (void(*)(void))trampoline, 1, this);
}

Coroutine& Coroutine::create_fiber(entry_t* entry, void* arg) {
  return LocalThread::get().create_fiber(*entry, arg);
}

void Coroutine::reset(entry_t* entry, void* arg) {
	this->entry = entry;
	this->arg = arg;
}

void Coroutine::run() volatile {
	Coroutine& current = *const_cast<Coroutine*>(thread.current_fiber);
	assert(&current != this);
	if (thread.delete_me) {
		assert(this != thread.delete_me);
		assert(&current != thread.delete_me);
		delete thread.delete_me;
		thread.delete_me = NULL;
	}
	thread.current_fiber = this;
	swapcontext(&current.context, const_cast<ucontext_t*>(&context));
}

void Coroutine::finish(Coroutine& next) {
	this->thread.fiber_did_finish(*this);
	thread.current_fiber = &next;
	swapcontext(&context, &next.context);
}

void* Coroutine::bottom() const {
	return (char*)&stack[0];
}

size_t Coroutine::size() const {
	return sizeof(Coroutine) + STACK_SIZE;
}

/**
 * v8 hooks. Trick v8threads.cc into thinking that coroutines are actually threads.
 */
namespace v8 { namespace internal {
	/**
	* ThreadHandle is just a handle to a thread. It's important to note that ThreadHandle does NOT
	* own the underlying thread. We shim it make IsSelf() respect fibers.
	*/
	class ThreadHandle {
		enum Kind { SELF, INVALID };

		/**
		* PlatformData is a class with a couple of non-virtual methods and a single platform-specific
		* handle. PlatformData is stored as pointer in the ThreadHandle class so we can add new members
		* to the end and no one will mess with those members.
		*/
		class PlatformData {
			public:
				pthread_t _;
				Coroutine* coroutine;
				void Initialize(Kind kind);
		};
		ThreadHandle(Kind kind);
		void Initialize(Kind kind);
		bool IsSelf() const;

		/**
		* ThreadHandle's first member is PlatformData* data_. Since all pointers have the same size
		* as long as v8 doesn't change the layout of this class this is safe.
		*/
		PlatformData* data;
	};

	/**
	* Thread is an abstract class which manages creating and running new threads. In most cases v8
	* won't actually create any new threads. Notable uses of the Thread class include the profiler,
	* debugger, and the preemption thread.
	*
	* All of the methods we override are static and are just simple wrappers around pthread
	* functions.
	*
	* Note that Thread extends ThreadHandle, but this has no bearing on our implementation.
	*/
	class Thread : public ThreadHandle {
		public:
			enum LocalStorageKey {};
			static LocalStorageKey CreateThreadLocalKey();
			static void DeleteThreadLocalKey(LocalStorageKey key);
			static void* GetThreadLocal(LocalStorageKey key);
			static void SetThreadLocal(LocalStorageKey key, void* value);
	};

	/**
	* Override the constructor to instantiate our own PlatformData.
	*/
	ThreadHandle::ThreadHandle(Kind kind) {
		data = new PlatformData;
		Initialize(kind);
	}

	void ThreadHandle::Initialize(Kind kind) {
		data->Initialize(kind);
		data->coroutine = kind == SELF ? &Coroutine::current() : NULL;
	}

	/**
	* Fool v8 into thinking it's in a new thread.
	*/
	bool ThreadHandle::IsSelf() const {
		return data->coroutine == &Coroutine::current();
	}

	/**
	* All thread-specific functions will just go to the fiber-local storage engine in this source
	* file.
	*/
	Thread::LocalStorageKey Thread::CreateThreadLocalKey() {
		did_hook_v8 = true;
		return static_cast<LocalStorageKey>(LocalThread::key_create(NULL));
	}

	void Thread::DeleteThreadLocalKey(LocalStorageKey key) {
		// Kidding me?
	}

	void* Thread::GetThreadLocal(Thread::LocalStorageKey key) {
		return LocalThread::get().get_specific(static_cast<pthread_key_t>(key));
	}

	void Thread::SetThreadLocal(Thread::LocalStorageKey key, void* value) {
		LocalThread::get().set_specific(static_cast<pthread_key_t>(key), value);
	}
}}

/**
 * Initialization of this library. By the time we make it here the heap should be good to go. Also
 * it's possible the TLS functions have been called, so we need to clean up that mess.
 */
class Loader {
	public: Loader() {
		pthread_key_create(&thread_key, LocalThread::free);

		// Undo fiber-shim so that child processes don't get shimmed as well. This also seems to prevent
		// this library from being loaded multiple times.
		setenv("DYLD_INSERT_LIBRARIES", "", 1);
		setenv("LD_PRELOAD", "", 1);
	}
};
Loader loader;
