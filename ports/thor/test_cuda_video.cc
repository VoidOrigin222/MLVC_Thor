#include <algorithm>
#include <cassert>
#include <cmath>
#include <memory>
#include <vector>

#include "mlvc/codec/tensor_data.h"
#include "mlvc/codec/translation_warp.h"
#include "mlvc/core/buffer.h"
#include "mlvc/core/status.h"
#include "mlvc/io/fp16_yuv444_to_nv12.h"
#include "mlvc/runtime/acl_runtime.h"
#include "mlvc/runtime/fp16_yuv444_to_nv12_acl.h"
#include "nvdec_reader.h"
namespace mlvc::io {
void BgrToFp16Yuv444Cuda(const void*, int, int, int, int, void*, cudaStream_t);
}
int main() {
  using mlvc::CheckAcl;
  using mlvc::DataType;
  using mlvc::DeviceBuffer;
  using mlvc::Error;
  using mlvc::MemoryLocation;
  using mlvc::TensorRtRuntime;
  using mlvc::TensorShape;
  using mlvc::TensorSpec;
  namespace codec = mlvc::codec;
  namespace io = mlvc::io;
  TensorRtRuntime runtime(0);
  constexpr int w = 10, h = 6, pw = 12, ph = 8, plane = pw * ph;
  std::vector<uint8_t> bgr(w * h * 3);
  for (size_t i = 0; i < bgr.size(); ++i) bgr[i] = static_cast<uint8_t>(i * 73);
  DeviceBuffer device_bgr(bgr.size()), device_frame(plane * 3 * 2);
  CheckAcl(cudaMemcpy(device_bgr.data(), bgr.data(), bgr.size(),
                      cudaMemcpyHostToDevice),
           "test upload");
  io::BgrToFp16Yuv444Cuda(device_bgr.data(), w, h, pw, ph, device_frame.data(),
                          runtime.stream());
  CheckAcl(cudaStreamSynchronize(runtime.stream()), "test convert");
  auto frame = codec::MakeTensor({1, 3, ph, pw}, DataType::kFloat16);
  CheckAcl(cudaMemcpy(frame.bytes.data(), device_frame.data(), frame.ByteSize(),
                      cudaMemcpyDeviceToHost),
           "test download");
  const auto* fp = reinterpret_cast<const uint16_t*>(frame.bytes.data());
  for (int y = 0; y < ph; ++y)
    for (int x = 0; x < pw; ++x) {
      int p = (std::min(y, h - 1) * w + std::min(x, w - 1)) * 3;
      float b = bgr[p] / 255.f, g = bgr[p + 1] / 255.f, r = bgr[p + 2] / 255.f;
      float luma = .2126f * r + .7152f * g + .0722f * b;
      float expected[] = {luma, .5f * (b - luma) / (1 - .0722f) + .5f,
                          .5f * (r - luma) / (1 - .2126f) + .5f};
      for (int c = 0; c < 3; ++c)
        assert(std::abs(codec::HalfBitsToFloat(fp[c * plane + y * pw + x]) -
                        expected[c]) < .0005f);
    }
  io::Nv12Layout layout{w, h, 14, 10};
  std::vector<uint8_t> golden;
  io::ConvertFp16Yuv444ToNv12Scalar(frame, layout, &golden);
  DeviceBuffer device_nv12(golden.size());
  Fp16Yuv444ToNv12Acl(device_frame.data(), frame.shape, layout,
                      device_nv12.data(), runtime.stream(), nullptr);
  CheckAcl(cudaStreamSynchronize(runtime.stream()), "test NV12");
  std::vector<uint8_t> actual(golden.size());
  CheckAcl(cudaMemcpy(actual.data(), device_nv12.data(), actual.size(),
                      cudaMemcpyDeviceToHost),
           "test NV12 download");
  assert(actual == golden);
  // NV12/P010/P016 planes with independent pitches, limited/full range,
  // chroma siting and edge padding. Scalar golden includes BGR8 rounding.
  for (int depth : std::vector<int>{8, 10, 12, 16})
    for (bool full : std::vector<bool>{false, true}) {
      bool wide = depth > 8;
      int yp = (w + 4) * (wide ? 2 : 1), up = (w + 8) * (wide ? 2 : 1);
      std::vector<uint8_t> ydata(yp * h, 0xee), udata(up * h / 2, 0xee);
      auto put = [&](std::vector<uint8_t>& data, int pitch, int x, int y,
                     int value) {
        if (wide)
          reinterpret_cast<uint16_t*>(data.data() + y * pitch)[x] =
              static_cast<uint16_t>(value << (16 - depth));
        else
          data[y * pitch + x] = static_cast<uint8_t>(value);
      };
      int scale = 1 << (depth - 8);
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          put(ydata, yp, x, y,
              (full ? 0 : 16) * scale + (x * 19 + y * 7) * scale);
      for (int y = 0; y < h / 2; ++y)
        for (int x = 0; x < w / 2; ++x) {
          put(udata, up, x * 2, y, (80 + x * 17 + y * 8) * scale);
          put(udata, up, x * 2 + 1, y, (160 - x * 11 + y * 9) * scale);
        }
      DeviceBuffer luma(ydata.size()), chroma(udata.size());
      CheckAcl(cudaMemcpy(luma.data(), ydata.data(), ydata.size(),
                          cudaMemcpyHostToDevice),
               "upload decode luma");
      CheckAcl(cudaMemcpy(chroma.data(), udata.data(), udata.size(),
                          cudaMemcpyHostToDevice),
               "upload decode chroma");
      for (int siting : std::vector<int>{1, 2, 3, 4, 5, 6}) {
        io::NvdecFrame surface;
        surface.luma = static_cast<const uint8_t*>(luma.data());
        surface.chroma = static_cast<const uint8_t*>(chroma.data());
        surface.luma_pitch = yp;
        surface.chroma_pitch = up;
        surface.width = w;
        surface.height = h;
        surface.depth = depth;
        surface.wide = wide;
        surface.full_range = full;
        surface.chroma_location = siting;
        io::Cuda420ToFp16Yuv444(surface, pw, ph, device_frame.data(),
                                runtime.stream());
        CheckAcl(cudaStreamSynchronize(runtime.stream()),
                 "test hardware surface conversion");
        CheckAcl(cudaMemcpy(frame.bytes.data(), device_frame.data(),
                            frame.ByteSize(), cudaMemcpyDeviceToHost),
                 "download hardware surface conversion");
        auto sample = [&](const std::vector<uint8_t>& data, int pitch, int x,
                          int y) {
          return wide ? static_cast<float>(reinterpret_cast<const uint16_t*>(
                                               data.data() + y * pitch)[x] >>
                                           (16 - depth))
                      : static_cast<float>(data[y * pitch + x]);
        };
        auto chroma = [&](float x, float y, int c) {
          x = std::clamp(x, 0.f, static_cast<float>(w / 2 - 1));
          y = std::clamp(y, 0.f, static_cast<float>(h / 2 - 1));
          int x0 = static_cast<int>(floor(x)), y0 = static_cast<int>(floor(y)),
              x1 = std::min(x0 + 1, w / 2 - 1),
              y1 = std::min(y0 + 1, h / 2 - 1);
          float dx = x - x0, dy = y - y0, a = sample(udata, up, x0 * 2 + c, y0),
                b = sample(udata, up, x1 * 2 + c, y0);
          float c0 = sample(udata, up, x0 * 2 + c, y1),
                d = sample(udata, up, x1 * 2 + c, y1);
          return (a + (b - a) * dx) * (1 - dy) + (c0 + (d - c0) * dx) * dy;
        };
        for (int y = 0; y < ph; ++y)
          for (int x = 0; x < pw; ++x) {
            int sx = std::min(x, w - 1), sy = std::min(y, h - 1);
            float cx = sx * .5f, cy = sy * .5f - .25f;
            if (siting == 2 || siting == 4 || siting == 6) cx -= .25f;
            if (siting == 3 || siting == 4) cy = sy * .5f;
            if (siting == 5 || siting == 6) cy = sy * .5f - .5f;
            float maximum = static_cast<float>((1 << depth) - 1),
                  luma = sample(ydata, yp, sx, sy);
            float u = (chroma(cx, cy, 0) - 128 * scale) /
                      (full ? maximum : static_cast<float>(224 * scale));
            float v = (chroma(cx, cy, 1) - 128 * scale) /
                      (full ? maximum : static_cast<float>(224 * scale));
            luma = full ? luma / maximum : (luma - 16 * scale) / (219 * scale);
            auto byte = [](float f) {
              return static_cast<float>(
                         std::nearbyint(std::clamp(f, 0.f, 1.f) * 255.f)) /
                     255.f;
            };
            float r = byte(luma + 1.402f * v), b = byte(luma + 1.772f * u),
                  g = byte(luma - .3441362862f * u - .7141362862f * v);
            float yy = .2126f * r + .7152f * g + .0722f * b;
            float expected[] = {yy, .5f * (b - yy) / (1 - .0722f) + .5f,
                                .5f * (r - yy) / (1 - .2126f) + .5f};
            auto* bits = reinterpret_cast<const uint16_t*>(frame.bytes.data());
            for (int c = 0; c < 3; ++c)
              assert(std::abs(
                         codec::HalfBitsToFloat(bits[c * plane + y * pw + x]) -
                         expected[c]) < .0007f);
          }
      }
    }
  for (auto shift : std::vector<std::pair<int, int>>{
           {0, 0}, {1, 2}, {-2, -1}, {pw - 1, ph - 1}, {-pw + 1, -ph + 1}}) {
    auto gpu = codec::MakeTensor({1, 3, ph, pw}, DataType::kFloat16);
    auto owner = std::make_shared<DeviceBuffer>(frame.ByteSize());
    CheckAcl(cudaMemcpy(owner->data(), frame.View().data(), frame.ByteSize(),
                        cudaMemcpyHostToDevice),
             "upload warp test");
    gpu.AttachExternalBuffer(owner->data(), owner->bytes(),
                             MemoryLocation::kAcl, owner);
    auto expected =
        codec::ShiftTensorPreserveBoundary(frame, shift.first, shift.second);
    codec::TensorData shifted;
    codec::ShiftTensorPreserveBoundaryInto(gpu, shift.first, shift.second,
                                           &shifted);
    std::vector<uint8_t> bytes(expected.ByteSize());
    CheckAcl(cudaMemcpy(bytes.data(), shifted.View().data(), bytes.size(),
                        cudaMemcpyDeviceToHost),
             "download warp test");
    assert(bytes == expected.bytes);
    const auto view = gpu.View();
    codec::ShiftTensorPreserveBoundaryInto(view, shift.first, shift.second,
                                           &gpu);
    CheckAcl(cudaMemcpy(bytes.data(), gpu.View().data(), bytes.size(),
                        cudaMemcpyDeviceToHost),
             "download inplace warp test");
    assert(bytes == expected.bytes);
  }
  bool rejected = false;
  try {
    io::BgrToFp16Yuv444Cuda(device_bgr.data(), w, h, w - 1, ph,
                            device_frame.data(), runtime.stream());
  } catch (const Error&) {
    rejected = true;
  }
  assert(rejected);
}
