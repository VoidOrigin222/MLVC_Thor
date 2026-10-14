extern "C" {
#include <libavformat/avformat.h>
}

#include <unistd.h>

#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>

#include "mlvc/io/dvpp_h264_encoder.h"
#include "mlvc/io/video_io.h"
#include "mlvc/runtime/acl_runtime.h"
#include "test_video_fixture.h"

int main() {
  using mlvc::CheckAcl;
  using mlvc::DataType;
  using mlvc::MemoryLocation;
  using mlvc::TensorRtRuntime;
  using mlvc::TensorSpec;
  namespace codec = mlvc::codec;
  namespace io = mlvc::io;
  namespace thor = mlvc::thor;
  TensorRtRuntime runtime(0);
  auto file = std::filesystem::temp_directory_path() /
              ("thor-nvdec-" + std::to_string(getpid()) + ".mp4");
  constexpr int w = 256, h = 256;
  thor::CreateHardwareTestVideo(file, &runtime, 12, 60, 24, 8);
  TensorSpec spec;
  spec.shape = {1, 3, h + 8, w};
  spec.dtype = DataType::kFloat16;
  for (double target : std::vector<double>{0, 30, 25}) {
    io::VideoFrameReader reader(file, "nvdec");
    reader.SetDeviceResident(true);
    reader.SetTargetFps(target);
    assert(reader.info().width == w && reader.info().height == h &&
           std::abs(reader.info().fps - (target ? target : 60)) < 1e-6);
    codec::TensorData frame;
    int count = 0;
    while (reader.ReadFrameAsTensor(spec, &frame)) {
      int index = target
                      ? static_cast<int>(std::ceil(count * 60 / target - 1e-8))
                      : count;
      uint16_t bits[2]{};
      CheckAcl(cudaMemcpy(bits, frame.View().data(), 2, cudaMemcpyDeviceToHost),
               "check NVDEC sample");
      assert(std::abs(codec::HalfBitsToFloat(bits[0]) -
                      (24 + index * 8) / 255.f) < .02f);
      CheckAcl(cudaMemcpy(bits + 1,
                          static_cast<uint8_t*>(frame.View().data()) +
                              static_cast<size_t>(h + 7) * w * 2,
                          2, cudaMemcpyDeviceToHost),
               "check NVDEC padding");
      assert(bits[0] == bits[1]);
      ++count;
    }
    assert(count == (target == 0 ? 12 : target == 30 ? 6 : 5));
    reader.Close();
    reader.Close();
    uint16_t value = 0;
    CheckAcl(cudaMemcpy(&value, frame.View().data(), 2, cudaMemcpyDeviceToHost),
             "check NVDEC lease after close");
  }
  {
    // Keep three independent GPU tensor slots alive while the worker runs
    // ahead. Closing early must cancel that worker without invalidating
    // retained slots.
    io::VideoFrameReader reader(file, "nvdec");
    reader.SetDeviceResident(true);
    codec::TensorData slots[3];
    for (auto& slot : slots) assert(reader.ReadFrameAsTensor(spec, &slot));
    reader.Close();
    for (int i = 0; i < 3; ++i) {
      uint16_t value = 0;
      CheckAcl(
          cudaMemcpy(&value, slots[i].View().data(), 2, cudaMemcpyDeviceToHost),
          "check retained NVDEC slots");
      assert(std::abs(codec::HalfBitsToFloat(value) - (24 + i * 8) / 255.f) <
             .02f);
    }
  }
  {
    io::VideoFrameReader reader(file, "nvdec");
    codec::TensorData frame;
    assert(reader.ReadFrameAsTensor(spec, &frame));
    assert(frame.View().location() == MemoryLocation::kCpu);
    assert(std::abs(codec::HalfBitsToFloat(
                        static_cast<const uint16_t*>(frame.View().data())[0]) -
                    24 / 255.f) < .02f);
  }
  std::filesystem::remove(file);
}
