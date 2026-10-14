#ifndef MLVC_PORTS_THOR_INCLUDE_MLVC_RUNTIME_THOR_TIMING_H_
#define MLVC_PORTS_THOR_INCLUDE_MLVC_RUNTIME_THOR_TIMING_H_

#include <cuda_runtime_api.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace mlvc::thor {
inline bool DetailedTimingEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("MLVC_THOR_TIMING");
    return value && std::string(value) == "1";
  }();
  return enabled;
}
inline int TimingWarmup() {
  static const int frames = [] {
    const char* value = std::getenv("MLVC_THOR_TIMING_WARMUP");
    return value ? std::max(0, std::atoi(value)) : 96;
  }();
  return frames;
}
using TimingClock = std::chrono::steady_clock;
inline double Milliseconds(TimingClock::time_point begin,
                           TimingClock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - begin).count();
}
struct TimingCollector {
  struct Entry {
    uint64_t seen = 0, count = 0;
    double sum = 0, maximum = 0;
  };
  std::mutex mutex;
  std::map<std::string, Entry> entries;
  ~TimingCollector() {
    for (const auto& [name, e] : entries)
      if (e.count) {
        std::cout << "thor_timing name=" << name << " samples=" << e.count
                  << " avg_ms=" << e.sum / e.count << " total_ms=" << e.sum
                  << " max_ms=" << e.maximum << '\n';
      }
  }
};
inline void RecordTiming(const std::string& name, double ms,
                         bool warmup = true) {
  if (!DetailedTimingEnabled()) return;
  static TimingCollector collector;
  std::lock_guard<std::mutex> lock(collector.mutex);
  auto& entry = collector.entries[name];
  if (entry.seen++ < static_cast<uint64_t>(warmup ? TimingWarmup() : 0)) return;
  ++entry.count;
  entry.sum += ms;
  entry.maximum = std::max(entry.maximum, ms);
}
class TimingScope {
 public:
  explicit TimingScope(std::string name, bool warmup = true)
      : enabled_(DetailedTimingEnabled()),
        name_(std::move(name)),
        warmup_(warmup) {
    if (enabled_) begin_ = TimingClock::now();
  }
  ~TimingScope() {
    if (enabled_)
      RecordTiming(name_, Milliseconds(begin_, TimingClock::now()), warmup_);
  }

 private:
  bool enabled_;
  std::string name_;
  bool warmup_;
  TimingClock::time_point begin_;
};
// Reuse events, and read them only after the caller's existing stream fence.
// Event spans measure elapsed stream time, including gaps/interference, not
// the sum of pure kernel execution time.
class TimingEvents {
 public:
  ~TimingEvents() {
    for (auto event : events_)
      if (event) cudaEventDestroy(event);
  }
  void Record(int index, cudaStream_t stream) {
    if (!DetailedTimingEnabled()) return;
    if (!events_[0])
      for (auto& event : events_) Check(cudaEventCreate(&event));
    Check(cudaEventRecord(events_[index], stream));
  }
  void Sample(const std::string& name, int begin, int end, bool warmup = true) {
    if (!DetailedTimingEnabled()) return;
    float ms = 0;
    Check(cudaEventElapsedTime(&ms, events_[begin], events_[end]));
    RecordTiming(name, ms, warmup);
  }

 private:
  static void Check(cudaError_t result) {
    if (result != cudaSuccess)
      throw std::runtime_error(cudaGetErrorString(result));
  }
  cudaEvent_t events_[4]{};
};
}  // namespace mlvc::thor

#endif  // MLVC_PORTS_THOR_INCLUDE_MLVC_RUNTIME_THOR_TIMING_H_
