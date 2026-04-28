// src/common/include/openems/common/thread_pool.h
#pragma once

#include "openems/common/thread_safe_queue.h"

#include <vector>
#include <thread>
#include <future>
#include <functional>
#include <atomic>

namespace openems::common {

class ThreadPool {
public:
  explicit ThreadPool(size_t num_threads);
  ~ThreadPool();

  // 提交任务并返回 future
  template <typename F>
  auto submit(F fn) -> std::future<decltype(fn())> {
    using R = decltype(fn());
    auto task = std::make_shared<std::packaged_task<R()>>(std::move(fn));
    auto future = task->get_future();
    queue_.push([task]() { (*task)(); });
    return future;
  }

  // 提交任务不关心返回值
  void submit_no_result(std::function<void()> fn);

  // 等待所有任务完成并退出
  void shutdown();

  size_t size() const { return threads_.size(); }

private:
  void worker_loop();

  std::vector<std::thread> threads_;
  ThreadSafeQueue<std::function<void()>> queue_;
  std::atomic<bool> stopped_{false};
};

} // namespace openems::common