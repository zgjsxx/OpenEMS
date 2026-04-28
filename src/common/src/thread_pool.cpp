// src/common/src/thread_pool.cpp
#include "openems/common/thread_pool.h"

namespace openems::common {

ThreadPool::ThreadPool(size_t num_threads) {
  for (size_t i = 0; i < num_threads; ++i) {
    threads_.emplace_back(&ThreadPool::worker_loop, this);
  }
}

ThreadPool::~ThreadPool() {
  if (!stopped_.load()) {
    shutdown();
  }
}

void ThreadPool::submit_no_result(std::function<void()> fn) {
  queue_.push(std::move(fn));
}

void ThreadPool::shutdown() {
  stopped_ = true;
  queue_.stop();
  for (auto& t : threads_) {
    if (t.joinable()) t.join();
  }
  threads_.clear();
}

void ThreadPool::worker_loop() {
  while (!stopped_.load()) {
    auto task = queue_.pop(100);
    if (task) {
      (*task)();
    }
  }
}

} // namespace openems::common