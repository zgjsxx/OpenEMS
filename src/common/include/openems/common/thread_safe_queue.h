// src/common/include/openems/common/thread_safe_queue.h
#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <chrono>

namespace openems::common {

template <typename T>
class ThreadSafeQueue {
public:
  ThreadSafeQueue() = default;
  ThreadSafeQueue(const ThreadSafeQueue&) = delete;
  ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;

  void push(T item) {
    {
      std::lock_guard lock(mutex_);
      queue_.push(std::move(item));
    }
    cond_.notify_one();
  }

  std::optional<T> pop(int timeout_ms = -1) {
    std::unique_lock lock(mutex_);
    if (timeout_ms < 0) {
      cond_.wait(lock, [this] { return !queue_.empty() || stopped_; });
    } else {
      auto ok = cond_.wait_for(
          lock, std::chrono::milliseconds(timeout_ms),
          [this] { return !queue_.empty() || stopped_; });
      if (!ok) return std::nullopt;
    }
    if (stopped_ || queue_.empty()) return std::nullopt;
    T item = std::move(queue_.front());
    queue_.pop();
    return item;
  }

  bool empty() const {
    std::lock_guard lock(mutex_);
    return queue_.empty();
  }

  size_t size() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
  }

  void stop() {
    {
      std::lock_guard lock(mutex_);
      stopped_ = true;
    }
    cond_.notify_all();
  }

  void reset() {
    std::lock_guard lock(mutex_);
    stopped_ = false;
    queue_ = {};
  }

private:
  std::queue<T> queue_;
  mutable std::mutex mutex_;
  std::condition_variable cond_;
  bool stopped_ = false;
};

} // namespace openems::common