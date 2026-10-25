// SPDX-License-Identifier: GPL-2.0-or-later
// Least-recently-used cache whose entries are computed exactly once, however many threads ask at the same time.
#pragma once

#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace autoadjust {

template <typename T>
class OnceCache {
 public:
  explicit OnceCache(size_t capacity) : capacity_(capacity) {}

  // Sets future to the entry for key; returns the promise when the caller has claimed it and must fulfil it.
  std::shared_ptr<std::promise<T>> claim(int key, std::shared_future<T>& future) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(key);
    if (it != map_.end()) {
      order_.splice(order_.begin(), order_, it->second);
      future = it->second->second;
      return nullptr;
    }
    auto promise = std::make_shared<std::promise<T>>();
    future = promise->get_future().share();
    order_.emplace_front(key, future);
    map_[key] = order_.begin();
    if (map_.size() > capacity_) {
      map_.erase(order_.back().first);
      order_.pop_back();
    }
    return promise;
  }

  // Drops a failed entry so the next request computes it again.
  void forget(int key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(key);
    if (it == map_.end()) return;
    order_.erase(it->second);
    map_.erase(it);
  }

  // Returns the value for key, computing it with make() unless another thread does or did.
  template <typename F>
  T get(int key, F&& make) {
    std::shared_future<T> future;
    if (auto promise = claim(key, future)) fulfil(key, *promise, make);
    return future.get();
  }

  template <typename F>
  void fulfil(int key, std::promise<T>& promise, F&& make) {
    try {
      promise.set_value(make());
    } catch (...) {
      promise.set_exception(std::current_exception());
      forget(key);
    }
  }

 private:
  size_t capacity_;
  std::mutex mutex_;
  std::list<std::pair<int, std::shared_future<T>>> order_;
  std::unordered_map<int, typename std::list<std::pair<int, std::shared_future<T>>>::iterator> map_;
};

}  // namespace autoadjust
