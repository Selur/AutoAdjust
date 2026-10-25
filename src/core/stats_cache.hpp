// Thread-safe least-recently-used cache of per-frame statistics.
#pragma once

#include <list>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "autoadjust.hpp"

namespace autoadjust {

class StatsCache {
 public:
  explicit StatsCache(size_t capacity = 1024) : capacity_(capacity) {}

  bool get(int k, FrameStats& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(k);
    if (it == map_.end()) return false;
    order_.splice(order_.begin(), order_, it->second);
    out = it->second->second;
    return true;
  }

  void put(int k, const FrameStats& s) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(k);
    if (it != map_.end()) return;
    order_.emplace_front(k, s);
    map_[k] = order_.begin();
    if (map_.size() > capacity_) {
      map_.erase(order_.back().first);
      order_.pop_back();
    }
  }

 private:
  size_t capacity_;
  std::mutex mutex_;
  std::list<std::pair<int, FrameStats>> order_;
  std::unordered_map<int, std::list<std::pair<int, FrameStats>>::iterator> map_;
};

}  // namespace autoadjust
