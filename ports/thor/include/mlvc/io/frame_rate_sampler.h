#ifndef MLVC_PORTS_THOR_INCLUDE_MLVC_IO_FRAME_RATE_SAMPLER_H_
#define MLVC_PORTS_THOR_INCLUDE_MLVC_IO_FRAME_RATE_SAMPLER_H_
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace mlvc::io {
// Sample source time, never decode speed or network arrival jitter. Each
// retained frame is the first source frame at/after the next output time.
class FrameRateSampler {
 public:
  FrameRateSampler(double source_fps, double target_fps)
      : source_fps_(source_fps),
        output_fps_(target_fps == 0 ? source_fps
                                    : std::min(source_fps, target_fps)) {
    if (!std::isfinite(source_fps) || source_fps <= 0 ||
        !std::isfinite(target_fps) || target_fps < 0)
      throw std::invalid_argument(
          "Frame rates must be finite; source > 0, target >= 0");
  }
  double output_fps() const { return output_fps_; }
  bool Keep(uint64_t source_index) {
    if (source_index != next_source_++)
      throw std::invalid_argument(
          "Sampling requires consecutive source frames");
    if (output_fps_ >= source_fps_) {
      ++kept_;
      return true;
    }
    const long double position =
        static_cast<long double>(source_index) * output_fps_ / source_fps_;
    if (position + 1e-9L < kept_) return false;
    ++kept_;
    return true;
  }

 private:
  double source_fps_, output_fps_;
  uint64_t next_source_ = 0, kept_ = 0;
};
}  // namespace mlvc::io

#endif  // MLVC_PORTS_THOR_INCLUDE_MLVC_IO_FRAME_RATE_SAMPLER_H_
