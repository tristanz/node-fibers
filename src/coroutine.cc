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
#define STACK_SIZE (1024 * 265)
#define MAX_POOL_SIZE 120

#include <iostream>
using namespace std;

static pthread_key_t thread_key;
static bool did_hook_v8 = false;

/**
 * LocalThread is only used internally for this library. It keeps track of all the fibers this
 * thread is currently running, and handles all the fiber-local storage logic. We store a handle to
 * a LocalThread object in TLS, and then it emulates TLS on top of fibers.
 */
class LocalThread {
  private:
    static size_t fls_key;
    size_t fiber_ids;
    stack<size_t> freed_fiber_ids;
    vector<vector<void*> > fls_data;
    vector<Coroutine*> fiber_pool;

    LocalThread() : fiber_ids(1), fls_data(1), delete_me(NULL) {
      current_fiber = new Coroutine(*this, 0);
    }

    ~LocalThread() {
      assert(freed_fiber_ids.size() == fiber_ids);
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
      vector<void*>& fiber_data = fls_data[fiber.id];
      do {
        did_delete = false;
        for (size_t ii = 0; ii < fiber_data.size(); ++ii) {
          if (fiber_data[ii]) {
            fiber_data[ii] = NULL;
          }
        }
      } while (did_delete);
    }

    void fiber_did_finish(Coroutine& fiber) {
      if (fiber_pool.size() < MAX_POOL_SIZE) {
        fiber_pool.push_back(&fiber);
      } else {
        freed_fiber_ids.push(fiber.id);
        coroutine_fls_dtor(fiber);
        // Can't delete right now because we're currently on this stack!
        assert(delete_me == NULL);
        delete_me = &fiber;
      }
    }

    Coroutine& create_fiber(Coroutine::entry_t& entry, void* arg) {
      size_t id;
      if (!fiber_pool.empty()) {
        Coroutine& fiber = *fiber_pool.back();
        fiber_pool.pop_back();
        fiber.reset(entry, arg);
        return fiber;
      }

      if (!freed_fiber_ids.empty()) {
        id = freed_fiber_ids.top();
        freed_fiber_ids.pop();
      } else {
        fls_data.resize(fls_data.size() + 1);
        id = fiber_ids++;
      }
      return *new Coroutine(*this, id, entry, arg);
    }

    void* get_specific(pthread_key_t key) {
      if (fls_data[current_fiber->id].size() <= key) {
        return NULL;
      }
      return fls_data[current_fiber->id][key];
    }

    void set_specific(pthread_key_t key, const void* data) {
      if (fls_data[current_fiber->id].size() <= key) {
        fls_data[current_fiber->id].resize(key + 1);
      }
      fls_data[current_fiber->id][key] = (void*)data;
    }

    static pthread_key_t key_create() {
      return fls_key++;
    }
};
size_t LocalThread::fls_key = 0;

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

Coroutine::Coroutine(LocalThread& t, size_t id) : thread(t), id(id) {}

Coroutine::Coroutine(LocalThread& t, size_t id, entry_t& entry, void* arg) :
  thread(t),
  id(id),
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
  thread.current_fiber = this;
  if (thread.delete_me) {
    assert(this != thread.delete_me);
    delete thread.delete_me;
    thread.delete_me = NULL;
  }
  swapcontext(&current.context, const_cast<ucontext_t*>(&context));
  thread.current_fiber = &current;
}

void Coroutine::finish(Coroutine& next) {
  this->thread.fiber_did_finish(*this);
  swapcontext(&context, &next.context);
}

void* Coroutine::bottom() const {
  return (char*)&stack[0] - STACK_SIZE;
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
        void Initialize(ThreadHandle::Kind kind);
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
    return static_cast<LocalStorageKey>(LocalThread::key_create());
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
            cout <<"hello\n";
    pthread_key_create(&thread_key, LocalThread::free);

    // Undo fiber-shim so that child processes don't get shimmed as well.
    setenv("DYLD_INSERT_LIBRARIES", "", 1);
    setenv("LD_PRELOAD", "", 1);
  }
};
Loader loader;
