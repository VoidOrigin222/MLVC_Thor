#include "mlvc/motion/dvpp_translation_estimator.h"

#include <chrono>
#include <iostream>
#include <stdexcept>

#include "mlvc/runtime/acl_runtime.h"
#include "mlvc/runtime/fp16_yuv444_to_nv12_acl.h"

namespace mlvc::motion {
namespace {

io::DvppH264EncoderConfig MotionProxyConfig(io::DvppH264EncoderConfig config) {
  config.persistent_channel = true;
  config.single_reference = true;
  // Host uploads use the encoder-owned DVPP pool, with no intermediate copy.
  config.zero_copy_input = false;
#ifdef MLVC_TENSORRT
  config.zero_copy_input = true;
#endif
  // Avoid the legacy RTSP publisher channel; each proxy owns its channel.
  if (config.channel == 0) config.channel = 1;
  return config;
}

}  // namespace

DvppTranslationEstimator::DvppTranslationEstimator(
    aclrtContext context, io::DvppH264EncoderConfig config,
    bool skip_loop_filter)
    : context_(context),
      layout_(config.layout),
      gop_(config.gop),
      encoder_(context, MotionProxyConfig(config)),
      extractor_(config.gop, skip_loop_filter, config.slice_count) {}

DvppTranslationEstimator::~DvppTranslationEstimator() {
#ifdef MLVC_TENSORRT
  if (gpu_frames_)
    std::cout << "motion_cuda_nv12_avg_ms=" << nv12_cuda_ms_ / gpu_frames_
              << "\n";
#endif
}

Translation DvppTranslationEstimator::Estimate(const codec::TensorData& input,
                                               uint64_t frame_index,
                                               bool random_access) {
  if (frame_index != next_frame_) {
    throw std::runtime_error(
        "DVPP motion proxy requires consecutive frames from zero");
  }
  const auto location = input.View().location();
#ifdef MLVC_TENSORRT
  if (location == MemoryLocation::kAcl) {
    const auto begin = std::chrono::steady_clock::now();
    const auto bytes = io::Nv12BufferSize(layout_);
    if (nv12_device_.bytes() != bytes) nv12_device_.Allocate(bytes);
    Fp16Yuv444ToNv12Acl(input.View().data(), input.shape, layout_,
                        nv12_device_.data(), nullptr, nullptr);
    CheckAcl(cudaStreamSynchronize(nullptr), "complete motion NV12 conversion");
    nv12_cuda_ms_ += std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - begin)
                         .count();
    ++gpu_frames_;
    return EstimateDevice(nv12_device_.data(), bytes, frame_index, nullptr,
                          random_access);
  }
#endif
  if ((location != MemoryLocation::kCpu &&
       location != MemoryLocation::kPinnedCpu) ||
      input.dtype != DataType::kFloat16 ||
      input.ByteSize() != input.shape.NumElements() * sizeof(uint16_t)) {
    throw std::runtime_error(
        "DVPP motion FP16 input must be a valid host tensor");
  }
  io::ConvertFp16Yuv444ToNv12(input, layout_, &nv12_host_);
  return EstimateNv12Host(nv12_host_.data(), nv12_host_.size(), frame_index,
                          random_access);
}

Translation DvppTranslationEstimator::EstimateNv12Host(const void* nv12_host,
                                                       std::size_t bytes,
                                                       uint64_t frame_index,
                                                       bool random_access) {
  if (frame_index != next_frame_) {
    throw std::runtime_error(
        "DVPP motion proxy requires consecutive frames from zero");
  }
  if (nv12_host == nullptr || bytes != io::Nv12BufferSize(layout_)) {
    throw std::runtime_error(
        "DVPP motion host NV12 must match the configured layout");
  }
  const bool keyframe = random_access || frame_index % gop_ == 0;
  auto packet = encoder_.EncodeHostNv12(nv12_host, bytes, keyframe);
  return DecodePacket(packet, frame_index, random_access);
}

Translation DvppTranslationEstimator::EstimateDevice(const void* nv12_device,
                                                     std::size_t bytes,
                                                     uint64_t frame_index,
                                                     aclrtEvent ready_event,
                                                     bool random_access) {
  if (frame_index != next_frame_) {
    throw std::runtime_error(
        "DVPP motion proxy requires consecutive frames from zero");
  }
  const bool keyframe = random_access || frame_index % gop_ == 0;
  auto packet =
      encoder_.EncodeDevice(nv12_device, bytes, ready_event, keyframe);
  return DecodePacket(packet, frame_index, random_access);
}

Translation DvppTranslationEstimator::DecodePacket(
    const std::vector<uint8_t>& packet, uint64_t frame_index,
    bool random_access) {
  auto result = extractor_.Decode(packet.data(), packet.size(), frame_index,
                                  random_access);
  ++next_frame_;
  return result;
}

}  // namespace mlvc::motion
