#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <vector>

#include "mlvc/io/frame_rate_sampler.h"
using mlvc::io::FrameRateSampler;
int main() {
  for (auto rates : std::vector<std::pair<double, double>>{{60, 30},
                                                           {50, 30},
                                                           {30, 25},
                                                           {59.94, 30},
                                                           {120, 25},
                                                           {25, 30},
                                                           {30, 30},
                                                           {15, 30},
                                                           {30, 0}}) {
    FrameRateSampler sampler(rates.first, rates.second);
    const double output =
        rates.second == 0 ? rates.first : std::min(rates.first, rates.second);
    assert(sampler.output_fps() == output);
    uint64_t kept = 0;
    for (uint64_t i = 0; i < 60000; ++i) {
      if (!sampler.Keep(i)) continue;
      // Every selected frame falls within one source frame of its output time.
      double error = i / rates.first - kept / output;
      assert(error >= -1e-8 && error < 1 / rates.first + 1e-8);
      ++kept;
    }
    assert(kept == static_cast<uint64_t>(
                       1 + std::floor(59999 * output / rates.first + 1e-9)));
  }
  FrameRateSampler sampler(50, 30);
  std::vector<int> indices;
  for (int i = 0; i < 10; ++i)
    if (sampler.Keep(i)) indices.push_back(i);
  assert((indices == std::vector<int>{0, 2, 4, 5, 7, 9}));
  for (double invalid : {-1., std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()}) {
    bool rejected = false;
    try {
      FrameRateSampler bad(30, invalid);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    assert(rejected);
  }
}
