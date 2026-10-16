#include <cuda_fp16.h>

#include <algorithm>
#include <memory>

#include "mlvc/codec/detail/stage/stage_runtime_state.h"
#include "mlvc/codec/tensor_data.h"
#include "mlvc/core/buffer.h"
#include "mlvc/core/status.h"
#include "mlvc/runtime/acl_runtime.h"
#include "mlvc/runtime/fp16_yuv444_to_nv12_acl.h"
#include "nvdec_reader.h"
namespace mlvc::codec {
namespace {
__global__ void ShiftBytes(const uint8_t* source, uint8_t* output, int count,
                           int width, int height, int element, int kx, int ky) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= count) return;
  int index = i / element, plane = width * height;
  int x = index % width, y = (index / width) % height;
  int sx = x - kx, sy = y - ky;
  int src =
      (sx >= 0 && sx < width && sy >= 0 && sy < height)
          ? ((index / plane) * plane + sy * width + sx) * element + i % element
          : i;
  output[i] = source[src];
}
}  // namespace
void ShiftCuda(const TensorView& source, int kx, int ky, TensorData* output) {
  const auto dims = source.shape().dims();
  Check(dims.size() >= 2 && source.data() && output,
        "Invalid CUDA reference shift");
  int width = dims.back(), height = dims[dims.size() - 2];
  Check(width > 0 && height > 0 && kx > -width && kx < width && ky > -height &&
            ky < height,
        "CUDA feature shift exceeds dimensions");
  const auto bytes = source.bytes();
  Check(bytes > 0 && bytes <= INT32_MAX, "CUDA feature tensor is too large");
  auto previous_owner = output->external_owner;
  if (output->external_location != MemoryLocation::kAcl ||
      output->external_bytes != bytes ||
      output->external_data == source.data() || !output->external_owner) {
    auto buffer = std::make_shared<DeviceBuffer>(bytes);
    output->shape = TensorShape(dims);
    output->dtype = source.dtype();
    output->AttachExternalBuffer(buffer->data(), bytes, MemoryLocation::kAcl,
                                 buffer);
  }
  output->shape = TensorShape(dims);
  output->dtype = source.dtype();
  const auto stream = static_cast<cudaStream_t>(g_acl_user_compute_stream);
  ShiftBytes<<<(bytes + 255) / 256, 256, 0, stream>>>(
      static_cast<const uint8_t*>(source.data()),
      static_cast<uint8_t*>(output->View().data()), bytes, width, height,
      ElementSize(source.dtype()), kx, ky);
  CheckAcl(cudaGetLastError(), "launch CUDA reference shift");
  CheckAcl(cudaStreamSynchronize(stream), "complete CUDA reference shift");
}
}  // namespace mlvc::codec
namespace mlvc::io {
namespace {
__device__ float SurfaceSample(const uint8_t* plane, int pitch, int x, int y,
                               bool wide, int depth) {
  const auto* row = plane + y * pitch;
  return wide ? static_cast<float>(reinterpret_cast<const uint16_t*>(row)[x] >>
                                   (16 - depth))
              : static_cast<float>(row[x]);
}
__device__ float ChromaSample(const uint8_t* plane, int pitch, int width,
                              int height, float x, float y, int component,
                              bool wide, int depth) {
  x = fminf(static_cast<float>(width - 1), fmaxf(0.f, x));
  y = fminf(static_cast<float>(height - 1), fmaxf(0.f, y));
  int x0 = static_cast<int>(floorf(x)), y0 = static_cast<int>(floorf(y)),
      x1 = min(x0 + 1, width - 1), y1 = min(y0 + 1, height - 1);
  float dx = x - x0, dy = y - y0;
  float a = SurfaceSample(plane, pitch, x0 * 2 + component, y0, wide, depth);
  float b = SurfaceSample(plane, pitch, x1 * 2 + component, y0, wide, depth);
  float c = SurfaceSample(plane, pitch, x0 * 2 + component, y1, wide, depth);
  float d = SurfaceSample(plane, pitch, x1 * 2 + component, y1, wide, depth);
  return (a + (b - a) * dx) * (1.f - dy) + (c + (d - c) * dx) * dy;
}
__device__ float Bgr8Compatible(float x) {
  return static_cast<float>(__float2int_rn(fminf(1.f, fmaxf(0.f, x)) * 255.f)) /
         255.f;
}
__global__ void Decoded420ToFp16(const uint8_t* yplane, const uint8_t* uvplane,
                                 int ypitch, int uvpitch, int width, int height,
                                 int pw, int ph, bool wide, int depth,
                                 bool full, int location, __half* output) {
  int i = blockIdx.x * blockDim.x + threadIdx.x, plane = pw * ph;
  if (i >= plane) return;
  int x = min(i % pw, width - 1), y = min(i / pw, height - 1);
  float cx = x * .5f, cy = y * .5f - .25f;
  if (location == 2 || location == 4 || location == 6) cx -= .25f;
  if (location == 3 || location == 4) cy = y * .5f;
  if (location == 5 || location == 6) cy = y * .5f - .5f;
  float scale = static_cast<float>(1 << (depth - 8)),
        maximum = static_cast<float>((1 << depth) - 1);
  float y_value = SurfaceSample(yplane, ypitch, x, y, wide, depth);
  float cb_value = ChromaSample(uvplane, uvpitch, width / 2, height / 2, cx, cy,
                                0, wide, depth);
  float cr_value = ChromaSample(uvplane, uvpitch, width / 2, height / 2, cx, cy,
                                1, wide, depth);
  y_value =
      full ? y_value / maximum : (y_value - 16.f * scale) / (219.f * scale);
  cb_value = (cb_value - 128.f * scale) / (full ? maximum : 224.f * scale);
  cr_value = (cr_value - 128.f * scale) / (full ? maximum : 224.f * scale);
  // Match the existing OpenCV/sws default BT.601 -> BGR8 conversion, then
  // the model's BT.709 full-range contract. No BGR image is materialized.
  float r = Bgr8Compatible(y_value + 1.402f * cr_value);
  float b = Bgr8Compatible(y_value + 1.772f * cb_value);
  float g = Bgr8Compatible(y_value - .3441362862f * cb_value -
                           .7141362862f * cr_value);
  float luma = .2126f * r + .7152f * g + .0722f * b;
  output[i] = __float2half_rn(luma);
  output[plane + i] = __float2half_rn(.5f * (b - luma) / (1.f - .0722f) + .5f);
  output[2 * plane + i] =
      __float2half_rn(.5f * (r - luma) / (1.f - .2126f) + .5f);
}
__global__ void BgrToYuv(const uint8_t* bgr, int width, int height,
                         int padded_width, int padded_height, __half* output) {
  const int i = blockIdx.x * blockDim.x + threadIdx.x;
  const int plane = padded_width * padded_height;
  if (i >= plane) return;
  const int x = min(i % padded_width, width - 1),
            y = min(i / padded_width, height - 1);
  const uint8_t* pixel = bgr + (y * width + x) * 3;
  const float b = pixel[0] / 255.0f, g = pixel[1] / 255.0f,
              r = pixel[2] / 255.0f;
  const float luma = .2126f * r + .7152f * g + .0722f * b;
  output[i] = __float2half_rn(luma);
  output[plane + i] = __float2half_rn(.5f * (b - luma) / (1.f - .0722f) + .5f);
  output[2 * plane + i] =
      __float2half_rn(.5f * (r - luma) / (1.f - .2126f) + .5f);
}
}  // namespace
void Cuda420ToFp16Yuv444(const NvdecFrame& frame, int pw, int ph, void* output,
                         cudaStream_t stream) {
  Check(frame.width > 0 && frame.height > 0 && frame.width % 2 == 0 &&
            frame.height % 2 == 0 && pw >= frame.width && ph >= frame.height &&
            frame.depth >= 8 && frame.depth <= 16 && frame.luma &&
            frame.chroma && output &&
            frame.luma_pitch >= frame.width * (frame.wide ? 2 : 1) &&
            frame.chroma_pitch >= frame.width * (frame.wide ? 2 : 1),
        "Invalid NVDEC CUDA conversion input");
  int count = pw * ph;
  Decoded420ToFp16<<<(count + 255) / 256, 256, 0, stream>>>(
      frame.luma, frame.chroma, frame.luma_pitch, frame.chroma_pitch,
      frame.width, frame.height, pw, ph, frame.wide, frame.depth,
      frame.full_range, frame.chroma_location, static_cast<__half*>(output));
  CheckAcl(cudaGetLastError(), "launch NVDEC-to-FP16 preprocessing");
}
void BgrToFp16Yuv444Cuda(const void* bgr, int width, int height,
                         int padded_width, int padded_height, void* output,
                         cudaStream_t stream) {
  Check(width > 0 && height > 0 && padded_width >= width &&
            padded_height >= height && bgr && output,
        "Invalid CUDA BGR input");
  int count = padded_width * padded_height;
  BgrToYuv<<<(count + 255) / 256, 256, 0, stream>>>(
      static_cast<const uint8_t*>(bgr), width, height, padded_width,
      padded_height, static_cast<__half*>(output));
  CheckAcl(cudaGetLastError(), "launch CUDA BGR preprocessing");
}
}  // namespace mlvc::io
namespace mlvc {
namespace {
__device__ uint8_t Byte(float value) {
  return static_cast<uint8_t>(
      __float2int_rn(fminf(1.0f, fmaxf(0.0f, value)) * 255.0f));
}
__global__ void ConvertNv12(const __half* input, int input_width,
                            int input_height, int width, int height, int stride,
                            int storage_height, uint8_t* output) {
  const int i = blockIdx.x * blockDim.x + threadIdx.x;
  const int plane = input_width * input_height;
  const int ybytes = stride * storage_height;
  if (i < ybytes) {
    const int x = i % stride, y = i / stride;
    output[i] = (x < width && y < height)
                    ? Byte(__half2float(input[y * input_width + x]))
                    : 0;
  }
  if (i < ybytes / 4) {
    const int x = (i % (stride / 2)) * 2, y = (i / (stride / 2)) * 2;
    const int offset = ybytes + (y / 2) * stride + x;
    float u = 0, v = 0;
    if (x < width && y < height) {
      for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx) {
          const int p = (y + dy) * input_width + x + dx;
          u += __half2float(input[plane + p]);
          v += __half2float(input[2 * plane + p]);
        }
      output[offset] = Byte(u * .25f);
      output[offset + 1] = Byte(v * .25f);
    } else {
      output[offset] = output[offset + 1] = 0;
    }
  }
}
}  // namespace
bool Fp16Yuv444ToNv12AclAvailable() { return true; }
void Fp16Yuv444ToNv12Acl(const void* input, const TensorShape& shape,
                         const io::Nv12Layout& layout, void* output,
                         void* stream, Profiler*) {
  Check(shape.rank() == 4 && shape.dim(0) == 1 && shape.dim(1) == 3 &&
            shape.dim(2) >= layout.height && shape.dim(3) >= layout.width,
        "CUDA NV12 input shape is invalid");
  Check(input && output && layout.width > 0 && layout.height > 0 &&
            layout.width % 2 == 0 && layout.height % 2 == 0 &&
            layout.width_stride % 2 == 0 && layout.height_stride % 2 == 0 &&
            layout.width_stride >= layout.width &&
            layout.height_stride >= layout.height,
        "CUDA NV12 layout is invalid");
  int count = layout.width_stride * layout.height_stride;
  ConvertNv12<<<(count + 255) / 256, 256, 0,
                static_cast<cudaStream_t>(stream)>>>(
      static_cast<const __half*>(input), shape.dim(3), shape.dim(2),
      layout.width, layout.height, layout.width_stride, layout.height_stride,
      static_cast<uint8_t*>(output));
  CheckAcl(cudaGetLastError(), "launch CUDA NV12 conversion");
}
void Fp16Yuv444ToNv12Acl(const void* input, const TensorShape& shape,
                         const io::Nv12Layout& layout, void* output,
                         void* stream, aclrtEvent start, aclrtEvent ready,
                         Profiler* profiler) {
  if (start)
    CheckAcl(cudaEventRecord(start, static_cast<cudaStream_t>(stream)),
             "record NV12 start");
  Fp16Yuv444ToNv12Acl(input, shape, layout, output, stream, profiler);
  if (ready)
    CheckAcl(cudaEventRecord(ready, static_cast<cudaStream_t>(stream)),
             "record NV12 completion");
}
}  // namespace mlvc
