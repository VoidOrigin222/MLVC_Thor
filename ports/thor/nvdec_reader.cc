#include "nvdec_reader.h"

#include <cuda.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/pixdesc.h>
}

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mlvc/core/status.h"
#include "mlvc/runtime/acl_runtime.h"

namespace mlvc::io {
namespace {
void CheckNvdecAv(int error, const char* where) {
  if (error >= 0) return;
  char text[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(error, text, sizeof(text));
  throw Error(std::string(where) + ": " + text);
}
int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
AVPixelFormat CudaFormat(AVCodecContext*, const AVPixelFormat* formats) {
  for (; *formats != AV_PIX_FMT_NONE; ++formats)
    if (*formats == AV_PIX_FMT_CUDA) return *formats;
  return AV_PIX_FMT_NONE;
}
}  // namespace
NvdecFrame::~NvdecFrame() {
  if (ready) {
    int previous = 0;
    cudaGetDevice(&previous);
    cudaSetDevice(device);
    cudaEventDestroy(ready);
    cudaSetDevice(previous);
  }
  av_frame_free(&frame);
}
class NvdecReader::Impl {
 public:
  AVFormatContext* format_ = nullptr;
  AVCodecContext* codec_ = nullptr;
  AVBufferRef* hardware_ = nullptr;
  AVPacket* packet_ = nullptr;
  cudaStream_t stream_ = nullptr;
  VideoInfo info_;
  std::ofstream timeline_;
  AVRational time_base_{};
  uint64_t timeline_index_ = 0;
  std::deque<std::shared_ptr<NvdecFrame>> prefetched_;
  int device_ = 0, video_ = -1, depth_ = 8;
  bool flushing_ = false, reported_ = false;
  std::atomic<bool> cancelled_{false};
  std::atomic<int64_t> deadline_{0};
  static int Interrupt(void* opaque) {
    auto& self = *static_cast<Impl*>(opaque);
    return self.cancelled_.load() ||
           (self.deadline_.load() && NowNs() > self.deadline_.load());
  }
  ~Impl() {
    cudaSetDevice(device_);
    if (stream_) cudaStreamSynchronize(stream_);
    av_packet_free(&packet_);
    avcodec_free_context(&codec_);
    avformat_close_input(&format_);
    av_buffer_unref(&hardware_);
    if (stream_) cudaStreamDestroy(stream_);
  }
  void Open(const std::filesystem::path& path) {
    CheckAcl(cudaGetDevice(&device_), "select NVDEC device");
    CheckAcl(cudaFree(nullptr), "initialize current CUDA context for NVDEC");
    format_ = avformat_alloc_context();
    Check(format_, "Cannot allocate NVDEC demuxer");
    format_->interrupt_callback = {Interrupt, this};
    AVDictionary* options = nullptr;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "rw_timeout", "3000000", 0);
    const bool live = path.string().rfind("rtsp://", 0) == 0 ||
                      path.string().rfind("rtsps://", 0) == 0;
    if (live) {
      // Detect the actual rate from decoded PTS below, rather than collecting
      // seconds of packets in the generic FPS probe. Retain the initial IDR:
      // discarding probe packets can require waiting another long GOP.
      format_->fps_probe_size = 0;
      av_dict_set(&options, "analyzeduration", "250000", 0);
      av_dict_set(&options, "probesize", "262144", 0);
    }
    deadline_ = NowNs() + 3'000'000'000LL;
    int result =
        avformat_open_input(&format_, path.string().c_str(), nullptr, &options);
    av_dict_free(&options);
    CheckNvdecAv(result, "open NVDEC video input");
    deadline_ = NowNs() + (live ? 8'000'000'000LL : 3'000'000'000LL);
    CheckNvdecAv(avformat_find_stream_info(format_, nullptr),
                 "read NVDEC stream information");
    deadline_ = 0;
    video_ =
        av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    CheckNvdecAv(video_, "find NVDEC video stream");
    auto* input = format_->streams[video_];
    time_base_ = input->time_base;
    if (const char* prefix = std::getenv("MLVC_THOR_INPUT_TIMELINE")) {
      static std::atomic<uint64_t> sequence{0};
      timeline_.open(std::string(prefix) + "-" + path.filename().string() +
                     "-" + std::to_string(sequence++) + ".csv");
      Check(timeline_.good(), "Cannot open NVDEC input timeline");
      timeline_ << "frame,pts_ns,read_ns\n";
    }
    const auto id = input->codecpar->codec_id;
    const char* name = id == AV_CODEC_ID_H264   ? "h264_cuvid"
                       : id == AV_CODEC_ID_HEVC ? "hevc_cuvid"
                                                : nullptr;
    Check(name,
          "NVDEC input requires H.264 or HEVC; software decode fallback is "
          "unavailable");
    const AVCodec* decoder = avcodec_find_decoder_by_name(name);
    Check(decoder, std::string("FFmpeg NVDEC decoder unavailable: ") + name);
    codec_ = avcodec_alloc_context3(decoder);
    Check(codec_, "Cannot allocate NVDEC codec");
    CheckNvdecAv(avcodec_parameters_to_context(codec_, input->codecpar),
                 "configure NVDEC");
    codec_->pkt_timebase = input->time_base;
    codec_->get_format = CudaFormat;
    codec_->thread_count = 1;
    // CUVID otherwise adds a four-frame display delay even for IP-only
    // streams. Retain reordering for inputs that declare B frames.
    if (live && codec_->has_b_frames == 0) {
      codec_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    }
    if (live) {
      std::cout << "input_nvdec_low_delay="
                << ((codec_->flags & AV_CODEC_FLAG_LOW_DELAY) != 0)
                << " has_b_frames=" << codec_->has_b_frames << '\n';
    }
    const auto* descriptor = av_pix_fmt_desc_get(
        static_cast<AVPixelFormat>(input->codecpar->format));
    if (descriptor) depth_ = descriptor->comp[0].depth;
    CheckNvdecAv(av_hwdevice_ctx_create(&hardware_, AV_HWDEVICE_TYPE_CUDA,
                                        std::to_string(device_).c_str(),
                                        nullptr, AV_CUDA_USE_CURRENT_CONTEXT),
                 "share CUDA context with NVDEC");
    // FFmpeg 6 CUVID maps/postprocesses surfaces on the legacy default stream
    // (CUVIDPROCPARAMS::output_stream is zero). Its copy stream must
    // participate in default-stream ordering; a nonblocking stream can copy
    // stale pixels before postprocessing completes when TensorRT is running
    // concurrently.
    CheckAcl(cudaStreamCreateWithFlags(&stream_, cudaStreamDefault),
             "create ordered NVDEC copy stream");
    auto* device_ctx = reinterpret_cast<AVHWDeviceContext*>(hardware_->data);
    auto* cuda_ctx = static_cast<AVCUDADeviceContext*>(device_ctx->hwctx);
    // FFmpeg borrows this stream for decoder surface copies. We own it.
    cuda_ctx->stream = reinterpret_cast<CUstream>(stream_);
    codec_->hw_device_ctx = av_buffer_ref(hardware_);
    Check(codec_->hw_device_ctx, "Cannot retain NVDEC device context");
    CheckNvdecAv(avcodec_open2(codec_, decoder, nullptr),
                 "open NVDEC hardware decoder");
    packet_ = av_packet_alloc();
    Check(packet_, "Cannot allocate NVDEC packet");
    double fps = av_q2d(live ? input->avg_frame_rate : input->r_frame_rate);
    if (!(fps > 0))
      fps = av_q2d(live ? input->r_frame_rate : input->avg_frame_rate);
    if (!(fps > 0)) fps = 30;
    info_ = {codec_->width, codec_->height, static_cast<int>(input->nb_frames),
             fps};
    Check(info_.width > 0 && info_.height > 0,
          "NVDEC input dimensions unavailable");
    if (live) {
      // RTSP r_frame_rate can be the H.264 field clock (26 for 13 FPS).
      // Use decoded frame PTS intervals before constructing the sampler.
      // Keep the probed frames so rate detection does not discard input.
      std::deque<std::shared_ptr<NvdecFrame>> frames;
      std::vector<int64_t> intervals;
      int64_t previous = AV_NOPTS_VALUE;
      for (int i = 0; i < 5; ++i) {
        std::shared_ptr<NvdecFrame> frame;
        Check(Read(&frame), "RTSP ended during frame-rate detection");
        const auto pts = frame->frame->best_effort_timestamp;
        if (previous != AV_NOPTS_VALUE && pts != AV_NOPTS_VALUE &&
            pts > previous)
          intervals.push_back(pts - previous);
        previous = pts;
        frames.push_back(std::move(frame));
      }
      if (!intervals.empty()) {
        std::sort(intervals.begin(), intervals.end());
        const double seconds =
            intervals[intervals.size() / 2] * av_q2d(time_base_);
        const double detected = 1.0 / seconds;
        const double rounded = std::round(detected);
        info_.fps = std::abs(detected - rounded) < 0.01 ? rounded : detected;
      }
      prefetched_ = std::move(frames);
      std::cout << "input_rate r_frame_rate=" << av_q2d(input->r_frame_rate)
                << " avg_frame_rate=" << av_q2d(input->avg_frame_rate)
                << " decoded_frame_fps=" << info_.fps << '\n';
    }
  }
  bool Read(std::shared_ptr<NvdecFrame>* output) {
    CheckAcl(cudaSetDevice(device_), "select NVDEC worker device");
    if (!prefetched_.empty()) {
      *output = std::move(prefetched_.front());
      prefetched_.pop_front();
      return true;
    }
    for (;;) {
      if (cancelled_) return false;
      auto frame = std::make_shared<NvdecFrame>();
      frame->device = device_;
      frame->frame = av_frame_alloc();
      Check(frame->frame, "Cannot allocate NVDEC frame");
      const int result = avcodec_receive_frame(codec_, frame->frame);
      if (result == AVERROR_EOF) return false;
      if (result >= 0) {
        // CUVID unmaps its source surface inside receive_frame. Complete the
        // private-stream copy before another receive/send can reuse it.
        CheckAcl(cudaStreamSynchronize(stream_),
                 "complete NVDEC surface copy before reuse");
        auto* decoded = frame->frame;
        if (timeline_.is_open()) {
          const int64_t pts = decoded->best_effort_timestamp;
          timeline_ << timeline_index_++ << ','
                    << (pts == AV_NOPTS_VALUE
                            ? -1
                            : av_rescale_q(pts, time_base_, {1, 1000000000}))
                    << ',' << NowNs() << '\n';
        }
        Check(decoded->format == AV_PIX_FMT_CUDA && decoded->hw_frames_ctx,
              "NVDEC must deliver a CUDA frame, without CPU fallback");
        auto* pool =
            reinterpret_cast<AVHWFramesContext*>(decoded->hw_frames_ctx->data);
        Check(pool->sw_format == AV_PIX_FMT_NV12 ||
                  pool->sw_format == AV_PIX_FMT_P010LE ||
                  pool->sw_format == AV_PIX_FMT_P016LE,
              "Unsupported NVDEC CUDA surface format");
        frame->wide = pool->sw_format != AV_PIX_FMT_NV12;
        frame->depth =
            frame->wide ? (pool->sw_format == AV_PIX_FMT_P010LE ? 10 : depth_)
                        : 8;
        Check(frame->depth >= 8 && frame->depth <= 16,
              "Invalid NVDEC sample depth");
        frame->luma = decoded->data[0];
        frame->chroma = decoded->data[1];
        frame->luma_pitch = decoded->linesize[0];
        frame->chroma_pitch = decoded->linesize[1];
        frame->width = decoded->width;
        frame->height = decoded->height;
        frame->full_range = decoded->color_range == AVCOL_RANGE_JPEG;
        frame->chroma_location = decoded->chroma_location;
        Check(frame->luma && frame->chroma, "NVDEC surface planes unavailable");
        CheckAcl(
            cudaEventCreateWithFlags(&frame->ready, cudaEventDisableTiming),
            "create NVDEC surface fence");
        CheckAcl(cudaEventRecord(frame->ready, stream_),
                 "record NVDEC surface readiness");
        if (!reported_) {
          std::cout
              << "input_video_decoder=nvdec codec=" << codec_->codec->name
              << " surface=" << av_get_pix_fmt_name(pool->sw_format)
              << " depth=" << frame->depth
              << " range=" << (frame->full_range ? "full" : "limited")
              << " width=" << frame->width << " height=" << frame->height
              << " coded_height=" << codec_->coded_height
              << " crop=" << decoded->crop_top << "," << decoded->crop_bottom
              << " preprocess=cuda-opencv-compatible-bgr8-to-bt709-fp16\n";
          reported_ = true;
        }
        *output = std::move(frame);
        return true;
      }
      if (result != AVERROR(EAGAIN))
        CheckNvdecAv(result, "receive NVDEC frame");
      Check(!flushing_, "NVDEC returned no frame after end-of-stream flush");
      for (;;) {
        deadline_ = NowNs() + 3'000'000'000LL;
        const int read = av_read_frame(format_, packet_);
        deadline_ = 0;
        if (cancelled_) return false;
        if (read == AVERROR_EOF) {
          flushing_ = true;
          CheckNvdecAv(avcodec_send_packet(codec_, nullptr),
                       "flush NVDEC decoder");
          break;
        }
        CheckNvdecAv(read, "read NVDEC packet");
        if (packet_->stream_index != video_) {
          av_packet_unref(packet_);
          continue;
        }
        const int sent = avcodec_send_packet(codec_, packet_);
        av_packet_unref(packet_);
        CheckNvdecAv(sent, "send NVDEC packet");
        break;
      }
    }
  }
};
NvdecReader::NvdecReader(const std::filesystem::path& path)
    : impl_(std::make_unique<Impl>()) {
  impl_->Open(path);
}
NvdecReader::~NvdecReader() = default;
const VideoInfo& NvdecReader::info() const { return impl_->info_; }
bool NvdecReader::Read(std::shared_ptr<NvdecFrame>* frame) {
  return impl_->Read(frame);
}
void NvdecReader::Cancel() { impl_->cancelled_ = true; }
void TraceNvdecInput(const NvdecFrame& frame, const void* tensor, size_t bytes,
                     cudaStream_t stream) {
  const char* path = std::getenv("MLVC_THOR_INPUT_DIGEST");
  if (!path || !*path) return;
  struct Diagnostic {
    using Digest = unsigned char* (*)(const unsigned char*, size_t,
                                      unsigned char*);
    void* crypto = dlopen("libcrypto.so.3", RTLD_NOW);
    Digest digest = nullptr;
    std::ofstream file;
    std::vector<unsigned char> raw, tensor;
    int index = 0;
    explicit Diagnostic(const char* path) : file(path) {
      if (crypto) digest = reinterpret_cast<Digest>(dlsym(crypto, "SHA256"));
      Check(digest && file.good(), "Cannot initialize NVDEC SHA256 diagnostic");
      file << "frame,pts,surface_sha256,tensor_sha256\n";
    }
    ~Diagnostic() {
      if (crypto) dlclose(crypto);
    }
    void Write(const std::vector<unsigned char>& input) {
      unsigned char hash[32];
      digest(input.data(), input.size(), hash);
      for (auto byte : hash)
        file << std::hex << std::setw(2) << std::setfill('0')
             << static_cast<int>(byte);
    }
  };
  static Diagnostic diagnostic(path);
  const size_t pitch = frame.width * (frame.wide ? 2 : 1),
               ybytes = pitch * frame.height;
  diagnostic.raw.resize(ybytes * 3 / 2);
  diagnostic.tensor.resize(bytes);
  CheckAcl(cudaMemcpy2DAsync(diagnostic.raw.data(), pitch, frame.luma,
                             frame.luma_pitch, pitch, frame.height,
                             cudaMemcpyDeviceToHost, stream),
           "diagnose NVDEC luma");
  CheckAcl(cudaMemcpy2DAsync(diagnostic.raw.data() + ybytes, pitch,
                             frame.chroma, frame.chroma_pitch, pitch,
                             frame.height / 2, cudaMemcpyDeviceToHost, stream),
           "diagnose NVDEC chroma");
  CheckAcl(cudaMemcpyAsync(diagnostic.tensor.data(), tensor, bytes,
                           cudaMemcpyDeviceToHost, stream),
           "diagnose NVDEC tensor");
  CheckAcl(cudaStreamSynchronize(stream),
           "complete NVDEC diagnostic downloads");
  diagnostic.file << std::dec << diagnostic.index++ << ',' << frame.frame->pts
                  << ',';
  diagnostic.Write(diagnostic.raw);
  diagnostic.file << ',';
  diagnostic.Write(diagnostic.tensor);
  diagnostic.file << '\n';
  Check(diagnostic.file.good(), "Cannot write NVDEC diagnostic");
}
}  // namespace mlvc::io
