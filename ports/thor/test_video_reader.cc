#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <vector>

#include "mlvc/io/video_io.h"
#include "mlvc/runtime/acl_runtime.h"
#include "test_video_fixture.h"
int main() {
  using mlvc::CheckAcl;
  using mlvc::DataType;
  using mlvc::TensorRtRuntime;
  using mlvc::TensorShape;
  using mlvc::TensorSpec;
  namespace codec = mlvc::codec;
  namespace io = mlvc::io;
  namespace thor = mlvc::thor;
  TensorRtRuntime runtime(0);
  auto file = std::filesystem::temp_directory_path() /
              ("thor-reader-" + std::to_string(getpid()) + ".mp4");
  thor::CreateHardwareTestVideo(file, &runtime, 4, 30, 20, 40);
  io::VideoFrameReader reader(file);
  assert(reader.info().width == 256 && reader.info().height == 256);
  TensorSpec spec;
  spec.shape = {1, 3, 264, 256};
  spec.dtype = DataType::kFloat16;
  codec::TensorData tensor;  // Same empty arena allocation as camera input.
  tensor.shape = TensorShape(spec.shape);
  tensor.dtype = spec.dtype;
  for (int i = 0; i < 4; ++i) {
    assert(reader.ReadFrameAsTensor(spec, &tensor));
    assert(tensor.ByteSize() == 3 * 264 * 256 * 2);
    auto* p = reinterpret_cast<const uint16_t*>(tensor.View().data());
    for (int y = 0; y < 264; ++y) {
      assert(std::abs(codec::HalfBitsToFloat(p[y * 256]) -
                      (20 + i * 40) / 255.f) < .02f);
      assert(p[y * 256] ==
             p[255 * 256]);  // Bottom padding replicates the edge.
    }
  }
  assert(!reader.ReadFrameAsTensor(spec, &tensor));
  reader.Close();
  reader.Close();
  assert(!reader.ReadFrameAsTensor(spec, &tensor));
  {
    io::VideoFrameReader gpu_reader(file);
    gpu_reader.SetDeviceResident(true);
    codec::TensorData first, second, third;
    assert(gpu_reader.ReadFrameAsTensor(spec, &first));
    assert(gpu_reader.ReadFrameAsTensor(spec, &second));
    assert(gpu_reader.ReadFrameAsTensor(spec, &third));
    assert(first.View().data() != second.View().data() &&
           second.View().data() != third.View().data());
    uint16_t value = 0;
    CheckAcl(cudaMemcpy(&value, first.View().data(), 2, cudaMemcpyDeviceToHost),
             "check queued GPU frame");
    assert(std::abs(codec::HalfBitsToFloat(value) - 20 / 255.f) < .02f);
    first.ClearExternalBuffer();
    assert(gpu_reader.ReadFrameAsTensor(spec, &first));
    gpu_reader.Close();
    CheckAcl(
        cudaMemcpy(&value, second.View().data(), 2, cudaMemcpyDeviceToHost),
        "check GPU lease after close");
    assert(std::abs(codec::HalfBitsToFloat(value) - 60 / 255.f) < .02f);
  }
  std::filesystem::remove(file);
  for (auto rates : std::vector<std::pair<double, double>>{
           {60, 30}, {50, 30}, {30, 25}, {25, 30}}) {
    thor::CreateHardwareTestVideo(file, &runtime, 12,
                                  static_cast<int>(rates.first), 20, 8);
    io::VideoFrameReader sampled(file);
    sampled.SetTargetFps(rates.second);
    double output = std::min(rates.first, rates.second);
    assert(std::abs(sampled.info().fps - output) < 1e-6);
    int count = 0;
    while (sampled.ReadFrameAsTensor(spec, &tensor)) {
      int selected =
          static_cast<int>(std::ceil(count * rates.first / output - 1e-8));
      const auto* pixels = static_cast<const uint16_t*>(tensor.View().data());
      assert(std::abs(codec::HalfBitsToFloat(pixels[0]) -
                      (20 + selected * 8) / 255.f) < .02f);
      ++count;
    }
    assert(count ==
           1 + static_cast<int>(std::floor(11 * output / rates.first + 1e-9)));
    sampled.Close();
    std::filesystem::remove(file);
  }
}
