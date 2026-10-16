#include <cuda.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/opt.h>
}

#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "mlvc/core/status.h"
#include "mlvc/io/dvpp_h264_encoder.h"
#include "mlvc/io/dvpp_jpeg_decoder.h"
#include "mlvc/io/dvpp_jpeg_encoder.h"
#include "mlvc/runtime/acl_runtime.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace mlvc::io {
namespace {
void AvCheck(int result, const char* label) {
  Check(result >= 0, std::string(label) + ": " + std::to_string(result));
}
cv::Mat Nv12Bgr(const void* bytes, const Nv12Layout& layout) {
  const auto* p = static_cast<const uint8_t*>(bytes);
  cv::Mat y(layout.height, layout.width, CV_8UC1, const_cast<uint8_t*>(p),
            layout.width_stride);
  cv::Mat uv(
      layout.height / 2, layout.width / 2, CV_8UC2,
      const_cast<uint8_t*>(p) +
          static_cast<size_t>(layout.width_stride) * layout.height_stride,
      layout.width_stride);
  cv::Mat bgr;
  cv::cvtColorTwoPlane(y, uv, bgr, cv::COLOR_YUV2BGR_NV12);
  return bgr;
}
}  // namespace
class DvppH264Encoder::Impl {
 public:
  DvppH264EncoderConfig config_;
  AVCodecContext* encoder_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVBufferRef* hw_device_ = nullptr;
  AVBufferRef* hw_pool_ = nullptr;
  cudaStream_t copy_stream_ = nullptr;
  int64_t index_ = 0;
  double copy_ms_ = 0, encode_ms_ = 0;
  Impl(aclrtContext context, DvppH264EncoderConfig c) : config_(c) {
    CheckAcl(aclrtSetCurrentContext(context), "select NVENC device");
    const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
    Check(codec != nullptr, "FFmpeg h264_nvenc encoder is unavailable");
    encoder_ = avcodec_alloc_context3(codec);
    Check(encoder_ != nullptr, "Cannot allocate NVENC context");
    encoder_->width = c.layout.width;
    encoder_->height = c.layout.height;
    if (c.zero_copy_input) {
      int device = 0;
      CheckAcl(cudaGetDevice(&device), "get NVENC CUDA device");
      AvCheck(av_hwdevice_ctx_create(&hw_device_, AV_HWDEVICE_TYPE_CUDA,
                                     std::to_string(device).c_str(), nullptr,
                                     AV_CUDA_USE_CURRENT_CONTEXT),
              "share current CUDA context with NVENC");
      CheckAcl(cudaStreamCreateWithFlags(&copy_stream_, cudaStreamNonBlocking),
               "create independent NVENC copy stream");
      auto* device_context =
          reinterpret_cast<AVHWDeviceContext*>(hw_device_->data);
      static_cast<AVCUDADeviceContext*>(device_context->hwctx)->stream =
          reinterpret_cast<CUstream>(copy_stream_);
      hw_pool_ = av_hwframe_ctx_alloc(hw_device_);
      Check(hw_pool_ != nullptr, "Cannot allocate CUDA frame pool");
      auto* pool = reinterpret_cast<AVHWFramesContext*>(hw_pool_->data);
      pool->format = AV_PIX_FMT_CUDA;
      pool->sw_format = AV_PIX_FMT_NV12;
      pool->width = c.layout.width;
      pool->height = c.layout.height;
      AvCheck(av_hwframe_ctx_init(hw_pool_), "initialize CUDA NV12 frame pool");
      encoder_->hw_frames_ctx = av_buffer_ref(hw_pool_);
    }
    encoder_->pix_fmt = c.zero_copy_input ? AV_PIX_FMT_CUDA : AV_PIX_FMT_NV12;
    encoder_->time_base = {1, static_cast<int>(c.fps)};
    encoder_->framerate = {static_cast<int>(c.fps), 1};
    encoder_->gop_size = c.gop;
    encoder_->max_b_frames = 0;
    encoder_->bit_rate = c.bitrate;
    encoder_->color_range = AVCOL_RANGE_JPEG;
    encoder_->colorspace = AVCOL_SPC_BT709;
    encoder_->color_primaries = AVCOL_PRI_BT709;
    encoder_->color_trc = AVCOL_TRC_BT709;
    encoder_->refs = c.single_reference ? 1 : 0;
    encoder_->slices = c.slice_count;
    av_opt_set(encoder_->priv_data, "preset", "p1", 0);
    av_opt_set(encoder_->priv_data, "tune", "ull", 0);
    av_opt_set(encoder_->priv_data, "zerolatency", "1", 0);
    av_opt_set(encoder_->priv_data, "delay", "0", 0);
    av_opt_set(encoder_->priv_data, "rc-lookahead", "0", 0);
    av_opt_set(encoder_->priv_data, "forced-idr", "1", 0);
    if (c.single_reference) av_opt_set(encoder_->priv_data, "dpb_size", "1", 0);
    AvCheck(avcodec_open2(encoder_, codec, nullptr), "open NVENC");
    frame_ = av_frame_alloc();
    Check(frame_ != nullptr, "Cannot allocate NVENC frame");
    frame_->format = encoder_->pix_fmt;
    frame_->width = encoder_->width;
    frame_->height = encoder_->height;
    frame_->color_range = encoder_->color_range;
    frame_->colorspace = encoder_->colorspace;
    frame_->color_primaries = encoder_->color_primaries;
    frame_->color_trc = encoder_->color_trc;
    if (hw_pool_)
      AvCheck(av_hwframe_get_buffer(hw_pool_, frame_, 0),
              "allocate NVENC GPU surface");
    else
      AvCheck(av_frame_get_buffer(frame_, 32), "allocate NVENC input");
  }
  ~Impl() {
    if (index_)
      std::cout << "nvenc_input_copy_avg_ms=" << copy_ms_ / index_
                << "\nnvenc_encode_avg_ms=" << encode_ms_ / index_ << "\n";
    if (frame_) av_frame_free(&frame_);
    if (encoder_) avcodec_free_context(&encoder_);
    av_buffer_unref(&hw_pool_);
    av_buffer_unref(&hw_device_);
    if (copy_stream_) cudaStreamDestroy(copy_stream_);
  }
  std::vector<uint8_t> Encode(const void* pixels, size_t bytes, bool keyframe,
                              bool device = false) {
    const auto t0 = std::chrono::steady_clock::now();
    Check(bytes >= Nv12BufferSize(config_.layout), "NVENC input is truncated");
    AvCheck(av_frame_make_writable(frame_), "make NVENC input writable");
    const auto* p = static_cast<const uint8_t*>(pixels);
    if (hw_pool_) {
      const auto kind =
          device ? cudaMemcpyDeviceToDevice : cudaMemcpyHostToDevice;
      CheckAcl(
          cudaMemcpy2DAsync(frame_->data[0], frame_->linesize[0], p,
                            config_.layout.width_stride, config_.layout.width,
                            config_.layout.height, kind, copy_stream_),
          "fill NVENC CUDA luma");
      p += static_cast<size_t>(config_.layout.width_stride) *
           config_.layout.height_stride;
      CheckAcl(
          cudaMemcpy2DAsync(frame_->data[1], frame_->linesize[1], p,
                            config_.layout.width_stride, config_.layout.width,
                            config_.layout.height / 2, kind, copy_stream_),
          "fill NVENC CUDA chroma");
      CheckAcl(cudaStreamSynchronize(copy_stream_),
               "complete NVENC input copy");
    } else {
      Check(!device, "Host NVENC path cannot consume a CUDA pointer");
      for (int row = 0; row < config_.layout.height; ++row)
        std::memcpy(frame_->data[0] + row * frame_->linesize[0],
                    p + row * config_.layout.width_stride,
                    config_.layout.width);
      p += static_cast<size_t>(config_.layout.width_stride) *
           config_.layout.height_stride;
      for (int row = 0; row < config_.layout.height / 2; ++row)
        std::memcpy(frame_->data[1] + row * frame_->linesize[1],
                    p + row * config_.layout.width_stride,
                    config_.layout.width);
    }
    const auto t1 = std::chrono::steady_clock::now();
    copy_ms_ += std::chrono::duration<double, std::milli>(t1 - t0).count();
    frame_->pts = index_++;
    frame_->pict_type = keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    AvCheck(avcodec_send_frame(encoder_, frame_), "send NVENC frame");
    AVPacket* packet = av_packet_alloc();
    Check(packet != nullptr, "Cannot allocate NVENC packet");
    int result = avcodec_receive_packet(encoder_, packet);
    if (result < 0) {
      av_packet_free(&packet);
      AvCheck(result, "receive NVENC frame");
    }
    encode_ms_ += std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t1)
                      .count();
    std::vector<uint8_t> output(packet->data, packet->data + packet->size);
    av_packet_free(&packet);
    return output;
  }
};
DvppH264Encoder::DvppH264Encoder(aclrtContext context,
                                 DvppH264EncoderConfig config)
    : impl_(std::make_unique<Impl>(context, config)) {}
DvppH264Encoder::~DvppH264Encoder() = default;
std::vector<uint8_t> DvppH264Encoder::EncodeHostNv12(const void* p,
                                                     size_t bytes, bool key) {
  return impl_->Encode(p, bytes, key);
}
std::vector<uint8_t> DvppH264Encoder::EncodeDevice(const void* p, size_t bytes,
                                                   aclrtEvent ready, bool key) {
  if (ready) CheckAcl(cudaEventSynchronize(ready), "wait NVENC input");
  if (impl_->hw_pool_) return impl_->Encode(p, bytes, key, true);
  std::vector<uint8_t> host(bytes);
  CheckAcl(cudaMemcpy(host.data(), p, bytes, cudaMemcpyDeviceToHost),
           "copy NVENC input");
  return impl_->Encode(host.data(), bytes, key);
}
class DvppJpegEncoder::Impl {
 public:
  Nv12Layout layout_;
  uint32_t quality_;
  Impl(Nv12Layout l, uint32_t q) : layout_(l), quality_(q) {}
};
DvppJpegEncoder::DvppJpegEncoder(aclrtContext, Nv12Layout l, uint32_t q)
    : impl_(std::make_unique<Impl>(l, q)) {}
DvppJpegEncoder::~DvppJpegEncoder() = default;
std::vector<uint8_t> DvppJpegEncoder::Encode(const std::vector<uint8_t>& input,
                                             DvppJpegTiming*) {
  Check(input.size() >= Nv12BufferSize(impl_->layout_), "JPEG input truncated");
  std::vector<uint8_t> output;
  Check(cv::imencode(
            ".jpg", Nv12Bgr(input.data(), impl_->layout_), output,
            {cv::IMWRITE_JPEG_QUALITY, static_cast<int>(impl_->quality_)}),
        "JPEG encode failed");
  return output;
}
std::vector<uint8_t> DvppJpegEncoder::EncodeDevice(const void* p, size_t bytes,
                                                   aclrtEvent event,
                                                   DvppJpegTiming* timing) {
  if (event) CheckAcl(cudaEventSynchronize(event), "wait JPEG input");
  std::vector<uint8_t> input(bytes);
  CheckAcl(cudaMemcpy(input.data(), p, bytes, cudaMemcpyDeviceToHost),
           "copy JPEG input");
  return Encode(input, timing);
}
class DvppJpegDecoder::Impl {
 public:
  DvppJpegDecodeConfig config_;
  explicit Impl(DvppJpegDecodeConfig c) : config_(c) {}
};
DvppJpegDecoder::DvppJpegDecoder(aclrtContext, DvppJpegDecodeConfig c)
    : impl_(std::make_unique<Impl>(c)) {}
DvppJpegDecoder::~DvppJpegDecoder() = default;
std::vector<uint8_t> DvppJpegDecoder::Decode(
    const std::vector<uint8_t>& input) {
  cv::Mat bgr = cv::imdecode(input, cv::IMREAD_COLOR);
  Check(!bgr.empty(), "JPEG decode failed");
  Check(bgr.cols == impl_->config_.width && bgr.rows == impl_->config_.height,
        "JPEG dimensions differ from stream");
  cv::Mat i420;
  cv::cvtColor(bgr, i420, cv::COLOR_BGR2YUV_I420);
  size_t y = static_cast<size_t>(bgr.cols) * bgr.rows;
  std::vector<uint8_t> output(y * 3 / 2);
  std::memcpy(output.data(), i420.data, y);
  for (size_t i = 0; i < y / 4; ++i) {
    output[y + 2 * i] = i420.data[y + i];
    output[y + 2 * i + 1] = i420.data[y + y / 4 + i];
  }
  return output;
}
DvppJpegDecodedFrame DvppJpegDecoder::DecodeDevice(const void* p,
                                                   size_t bytes) {
  const auto* data = static_cast<const uint8_t*>(p);
  auto host = Decode(std::vector<uint8_t>(data, data + bytes));
  void* device = nullptr;
  CheckAcl(cudaMalloc(&device, host.size()), "allocate JPEG decoded frame");
  std::shared_ptr<void> owner(device, [](void* value) { cudaFree(value); });
  CheckAcl(cudaMemcpy(device, host.data(), host.size(), cudaMemcpyHostToDevice),
           "upload JPEG decoded frame");
  return DvppJpegDecodedFrame(owner, host.size(), impl_->config_.width,
                              impl_->config_.height);
}
DvppJpegDecodedFrame DvppJpegDecoder::DecodeDevice(
    const std::vector<uint8_t>& input) {
  return DecodeDevice(input.data(), input.size());
}
}  // namespace mlvc::io
