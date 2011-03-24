#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE
#endif

#include <pthread.h>
#include <vector>

class Coroutine {
	public:
		typedef void(entry_t)(void*);

	private:
		static size_t stack_size;
		static std::vector<Coroutine*> fiber_pool;
		static pthread_key_t thread_key;
		static pthread_mutex_t mutex;

		bool started;
		entry_t* entry;
		void* arg;
		pthread_cond_t cond;
		void* base;

		static void trampoline(Coroutine& that);
		~Coroutine() {}

		/**
		 * Constructor for currently running "fiber". This is really just original thread, but we
		 * need a way to get back into the main thread after yielding to a fiber. Basically this
		 * shouldn't be called from anywhere.
		 */
		Coroutine();

		/**
		 * This constructor will actually create a new fiber context. Execution does not begin
		 * until you call run() for the first time.
		 */
		Coroutine(entry_t& entry, void* arg);

		/**
		 * Resets the context of this coroutine from the start. Used to recyle old coroutines.
		 */
		void reset(entry_t* entry, void* arg);

		/**
		 * Makes this coroutine runnable.
		 */
		void make_runnable();

	public:
		/**
		 * Initialize the coroutine library. This will be called automatically; don't call manually!
		 */
		static void init();

		/**
		 * Returns the currently-running fiber.
		 */
		static Coroutine& current();

		/**
		 * Create a new fiber.
		 */
		static Coroutine& create_fiber(entry_t* entry, void* arg = NULL);

		/**
		 * Is Coroutine-local storage via pthreads enabled? The Coroutine library should work fine
		 * without this, but programs that are not aware of coroutines may panic if they make
		 * assumptions about the stack. In order to enable this you must LD_PRELOAD (or equivalent)
		 * this library.
		 */
		static const bool is_local_storage_enabled();

		/**
		 * Set the size of coroutines created by this library. Since coroutines are pooled the stack
		 * size is global instead of per-coroutine.
		 */
		static void set_stack_size(size_t size);

		/**
		 * Start or resume execution in this fiber. Note there is no explicit yield() function,
		 * you must manually run another fiber.
		 */
		void run();

		/**
		 * Finish this coroutine.. This will halt execution of this coroutine and resume execution
		 * of `next`. If you do not call this function, and instead just return from `entry` the
		 * application will exit. This function may or may not actually return.
		 */
		void finish(Coroutine& next);

		/**
		 * Returns address of the lowest usable byte in this Coroutine's stack.
		 */
		void* bottom() const;

		/**
		 * Returns the size this Coroutine takes up in the heap.
		 */
		size_t size() const;
};
