#include "coroutine.h"
#include <assert.h>

#include <stdexcept>
#include <stack>
#include <vector>

#define MAX_POOL_SIZE 120

#include <iostream>
using namespace std;

size_t Coroutine::stack_size = 0;
vector<Coroutine*> Coroutine::fiber_pool;
pthread_key_t Coroutine::thread_key;
pthread_mutex_t Coroutine::mutex;

void Coroutine::init() {
	pthread_key_create(&thread_key, NULL);
	pthread_mutex_init(&mutex, NULL);
}

Coroutine::Coroutine() {
	assert(!pthread_getspecific(thread_key));
	pthread_setspecific(thread_key, this);
	pthread_cond_init(&cond, NULL);
	pthread_mutex_lock(&mutex);
}

Coroutine::Coroutine(entry_t& entry, void* arg) :
	started(false),
	entry(entry),
	arg(arg) {
	pthread_cond_init(&cond, NULL);
}

void Coroutine::trampoline(Coroutine &that) {
	// Estimate the bottom of this stack
	void* sp;
	that.base = &sp - stack_size + 32;

	// Put this handle into TLS
	assert(!pthread_getspecific(thread_key));
	pthread_setspecific(thread_key, &that);

	// Start the execution loop
	that.started = true;
	pthread_mutex_lock(&mutex);
	while (that.entry) {
		that.entry(const_cast<void*>(that.arg));
	}
	delete &that;
}

Coroutine& Coroutine::current() {
	Coroutine* self = static_cast<Coroutine*>(pthread_getspecific(thread_key));
	if (!self) {
		// Coroutine constructor will call pthread_setspecific
		self = new Coroutine;
	}
	return *self;
}

Coroutine& Coroutine::create_fiber(entry_t* entry, void* arg) {
	if (!fiber_pool.empty()) {
		Coroutine& fiber = *fiber_pool.back();
		fiber_pool.pop_back();
		fiber.reset(*entry, arg);
		return fiber;
	}
	return *new Coroutine(*entry, arg);
}

void Coroutine::make_runnable() {
	if (!started) {
		pthread_t thread;
		pthread_attr_t attrs;
		pthread_attr_init(&attrs);
		pthread_attr_setstacksize(&attrs, stack_size);
		assert(!pthread_create(&thread, &attrs, (void* (*)(void*))Coroutine::trampoline, (void*)this));
	} else {
		pthread_cond_signal(&cond);
	}
}

void Coroutine::run() {
	Coroutine& current = Coroutine::current();
	assert(&current != this);
	make_runnable();
	pthread_cond_wait(&current.cond, &mutex);
}

void Coroutine::finish(Coroutine& next) {
	next.make_runnable();
	if (fiber_pool.size() < MAX_POOL_SIZE) {
		fiber_pool.push_back(this);
		pthread_cond_wait(&cond, &mutex);
	} else {
		pthread_mutex_unlock(&mutex);
		pthread_cond_destroy(&cond);
		entry = NULL;
	}
}

const bool Coroutine::is_local_storage_enabled() {
	return true;
}

void Coroutine::set_stack_size(size_t size) {
	assert(!stack_size);
	stack_size = size;
}

void Coroutine::reset(entry_t* entry, void* arg) {
	this->entry = entry;
	this->arg = arg;
}

void* Coroutine::bottom() const {
	return base;
}

size_t Coroutine::size() const {
	return sizeof(Coroutine) + stack_size;
}

class Loader {
	public: Loader() {
		Coroutine::init();
	}
};
Loader loader;
