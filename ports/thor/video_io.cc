#include "mlvc/io/video_io.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/rational.h>
}

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mlvc/core/buffer.h"
#include "mlvc/core/status.h"
#include "mlvc/io/dvpp_h264_encoder.h"
#include "mlvc/io/dvpp_jpeg_decoder.h"
#include "mlvc/io/fp16_yuv444_to_nv12.h"
#include "mlvc/io/frame_rate_sampler.h"
#include "mlvc/runtime/acl_runtime.h"
#include "mlvc/runtime/fp16_yuv444_to_nv12_acl.h"
#include "mlvc/runtime/thor_timing.h"
#include "nvdec_reader.h"
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

namespace mlvc::io {
namespace codec = mlvc::codec;
namespace {
constexpr float kKr = 0.2126F;
constexpr float kKg = 0.7152F;
constexpr float kKb = 0.0722F;
std::string ToLower(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

uint8_t ClampToU8(float value) {
  value = std::min(std::max(value, 0.0F), 1.0F);
  return static_cast<uint8_t>(std::lrint(value * 255.0F));
}

cv::Mat TensorToBgrMat(const codec::TensorData& tensor, int width, int height) {
  Check(tensor.dtype == DataType::kFloat16,
        "decoded video output expects fp16 tensor");
  Check(tensor.shape.rank() == 4 && tensor.shape.dim(0) == 1 &&
            tensor.shape.dim(1) == 3,
        "decoded video output expects NCHW frame tensor");
  Check(tensor.shape.dim(2) >= height && tensor.shape.dim(3) >= width,
        "decoded frame tensor is smaller than requested video size");

  const int padded_height = static_cast<int>(tensor.shape.dim(2));
  const int padded_width = static_cast<int>(tensor.shape.dim(3));
  const auto view = tensor.View();
  Check(view.location() == MemoryLocation::kCpu ||
            view.location() == MemoryLocation::kPinnedCpu,
        "PNG/MP4 conversion requires a host tensor");
  const auto* input = static_cast<const uint16_t*>(view.data());
  cv::Mat frame(height, width, CV_8UC3);
  const std::size_t channel_stride =
      static_cast<std::size_t>(padded_height * padded_width);
  for (int y = 0; y < height; ++y) {
    auto* row = frame.ptr<cv::Vec3b>(y);
    for (int x = 0; x < width; ++x) {
      const std::size_t offset = static_cast<std::size_t>(y * padded_width + x);
      const float y_value = codec::HalfBitsToFloat(input[offset]);
      const float cb = codec::HalfBitsToFloat(input[channel_stride + offset]);
      const float cr =
          codec::HalfBitsToFloat(input[2 * channel_stride + offset]);
      const float r = y_value + (2.0F - 2.0F * kKr) * (cr - 0.5F);
      const float b = y_value + (2.0F - 2.0F * kKb) * (cb - 0.5F);
      const float g = (y_value - kKr * r - kKb * b) / kKg;
      row[x] = cv::Vec3b(ClampToU8(b), ClampToU8(g), ClampToU8(r));
    }
  }
  return frame;
}

std::string ShellQuote(const std::filesystem::path& path) {
  std::string quoted = "'";
  for (char c : path.string()) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted += c;
    }
  }
  quoted += "'";
  return quoted;
}

std::string BuildFfmpegCommand(const std::filesystem::path& output_path,
                               double fps, int width, int height, int crf,
                               const std::string& bitrate,
                               const std::string& preset) {
  std::ostringstream command;
  command << "ffmpeg -y -hide_banner -loglevel error"
          << " -f rawvideo -pix_fmt bgr24"
          << " -s " << width << "x" << height << " -r " << fps << " -i pipe:0"
          << " -an -c:v h264_nvenc -preset p1 -tune ull";
  if (bitrate.empty() || ToLower(bitrate) == "none") {
    command << " -qp " << crf;
  } else {
    command << " -b:v " << bitrate << " -maxrate " << bitrate
            << " -bufsize 5000k";
  }
  command << " -pix_fmt yuv420p -movflags +faststart "
          << ShellQuote(output_path);
  return command.str();
}

}  // namespace

struct RtspVideoPublisher::DirectState {
  AVFormatContext* format = nullptr;
  AVStream* stream = nullptr;
  AVCodecContext* encoder = nullptr;
  AVRational input_time_base{1, 30};
  AVRational stream_time_base{1, 90000};
  int64_t next_frame_pts = 0;
  bool h264_copy = false;
  bool header_written = false;
  std::string transport = "udp";

  ~DirectState() {
    if (encoder != nullptr) avcodec_free_context(&encoder);
    if (format != nullptr) {
      if (!(format->oformat->flags & AVFMT_NOFILE) && format->pb != nullptr) {
        avio_closep(&format->pb);
      }
      avformat_free_context(format);
    }
  }
};

std::string AvErrorText(int error) {
  char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(error, buffer, sizeof(buffer));
  return std::string(buffer);
}

void CheckAv(int error, const std::string& operation) {
  if (error < 0) {
    Check(false, operation + " failed: " + AvErrorText(error));
  }
}

AVRational FpsRational(double fps) {
  Check(fps > 0.0, "RTSP FPS must be positive");
  AVRational result = av_d2q(1.0 / fps, 100000);
  Check(result.num > 0 && result.den > 0, "RTSP frame time base is invalid");
  return result;
}

std::vector<std::pair<const uint8_t*, std::size_t>> FindAnnexBNals(
    const std::vector<uint8_t>& frame) {
  std::vector<std::pair<const uint8_t*, std::size_t>> nals;
  auto start_code = [&frame](std::size_t offset) -> std::size_t {
    if (offset + 3 <= frame.size() && frame[offset] == 0 &&
        frame[offset + 1] == 0 && frame[offset + 2] == 1) {
      return 3;
    }
    if (offset + 4 <= frame.size() && frame[offset] == 0 &&
        frame[offset + 1] == 0 && frame[offset + 2] == 0 &&
        frame[offset + 3] == 1) {
      return 4;
    }
    return 0;
  };
  std::size_t start = frame.size();
  std::size_t start_size = 0;
  for (std::size_t i = 0; i < frame.size();) {
    const std::size_t code_size = start_code(i);
    if (code_size == 0) {
      ++i;
      continue;
    }
    if (start != frame.size()) {
      const std::size_t nal_begin = start + start_size;
      if (i > nal_begin)
        nals.emplace_back(frame.data() + nal_begin, i - nal_begin);
    }
    start = i;
    start_size = code_size;
    i += code_size;
  }
  if (start != frame.size()) {
    const std::size_t nal_begin = start + start_size;
    if (frame.size() > nal_begin) {
      nals.emplace_back(frame.data() + nal_begin, frame.size() - nal_begin);
    }
  }
  const bool annexb_prefix =
      frame.size() >= 3 && frame[0] == 0 && frame[1] == 0 &&
      (frame[2] == 1 || (frame.size() >= 4 && frame[2] == 0 && frame[3] == 1));
  if (annexb_prefix) return nals;
  nals.clear();
  // DVPP may provide length-prefixed AVCC rather than Annex-B NAL units.
  std::size_t offset = 0;
  while (offset + 4 <= frame.size()) {
    const uint32_t nal_size = (static_cast<uint32_t>(frame[offset]) << 24) |
                              (static_cast<uint32_t>(frame[offset + 1]) << 16) |
                              (static_cast<uint32_t>(frame[offset + 2]) << 8) |
                              static_cast<uint32_t>(frame[offset + 3]);
    offset += 4;
    if (nal_size == 0 || nal_size > frame.size() - offset) {
      nals.clear();
      break;
    }
    const uint8_t nal_type = frame[offset] & 0x1f;
    if (nal_type == 0 || nal_type > 23) {
      nals.clear();
      break;
    }
    nals.emplace_back(frame.data() + offset, nal_size);
    offset += nal_size;
  }
  if (offset != frame.size()) nals.clear();
  return nals;
}

std::vector<uint8_t> BuildAvccExtradata(const std::vector<uint8_t>& frame) {
  const auto nals = FindAnnexBNals(frame);
  const uint8_t* sps = nullptr;
  std::size_t sps_size = 0;
  const uint8_t* pps = nullptr;
  std::size_t pps_size = 0;
  for (const auto& nal : nals) {
    if (nal.second == 0) continue;
    const int type = nal.first[0] & 0x1f;
    if (type == 7 && sps == nullptr) {
      sps = nal.first;
      sps_size = nal.second;
    } else if (type == 8 && pps == nullptr) {
      pps = nal.first;
      pps_size = nal.second;
    }
  }
  if (sps == nullptr || pps == nullptr || sps_size > 0xffff ||
      pps_size > 0xffff || sps_size < 4) {
    return {};
  }
  std::vector<uint8_t> extradata(11 + sps_size + pps_size);
  extradata[0] = 1;
  extradata[1] = sps[1];
  extradata[2] = sps[2];
  extradata[3] = sps[3];
  extradata[4] = 0xff;
  extradata[5] = 0xe1;
  extradata[6] = static_cast<uint8_t>(sps_size >> 8);
  extradata[7] = static_cast<uint8_t>(sps_size);
  std::memcpy(extradata.data() + 8, sps, sps_size);
  const std::size_t pps_header = 8 + sps_size;
  extradata[pps_header] = 1;
  extradata[pps_header + 1] = static_cast<uint8_t>(pps_size >> 8);
  extradata[pps_header + 2] = static_cast<uint8_t>(pps_size);
  std::memcpy(extradata.data() + pps_header + 3, pps, pps_size);
  return extradata;
}

std::vector<uint8_t> ConvertAnnexBToAvcc(const std::vector<uint8_t>& frame) {
  const auto nals = FindAnnexBNals(frame);
  if (nals.empty()) return frame;
  std::size_t total_size = 0;
  for (const auto& nal : nals) {
    Check(nal.second <= UINT32_MAX, "RTSP H.264 NAL unit is too large");
    total_size += 4 + nal.second;
  }
  std::vector<uint8_t> converted;
  converted.reserve(total_size);
  for (const auto& nal : nals) {
    const uint32_t size = static_cast<uint32_t>(nal.second);
    converted.push_back(static_cast<uint8_t>(size >> 24));
    converted.push_back(static_cast<uint8_t>(size >> 16));
    converted.push_back(static_cast<uint8_t>(size >> 8));
    converted.push_back(static_cast<uint8_t>(size));
    converted.insert(converted.end(), nal.first, nal.first + nal.second);
  }
  return converted;
}

void SetDirectH264Extradata(AVStream* stream,
                            const std::vector<uint8_t>& frame) {
  const std::vector<uint8_t> extradata = BuildAvccExtradata(frame);
  if (extradata.empty()) return;
  stream->codecpar->extradata = static_cast<uint8_t*>(
      av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
  Check(stream->codecpar->extradata != nullptr,
        "allocate RTSP H.264 extradata");
  std::memcpy(stream->codecpar->extradata, extradata.data(), extradata.size());
  stream->codecpar->extradata_size = static_cast<int>(extradata.size());
}

cv::Mat ConvertTensorToBgr(const codec::TensorData& tensor, int width,
                           int height) {
  return TensorToBgrMat(tensor, width, height);
}

bool IsCameraDevicePath(const std::filesystem::path& path) {
  const std::string value = path.string();
  return value.rfind("/dev/video", 0) == 0;
}

class VideoFrameReader::Impl {
 public:
  std::unique_ptr<NvdecReader> nvdec_;
  std::mutex mutex_;
  std::condition_variable available_, space_;
  struct CapturedFrame {
    std::shared_ptr<NvdecFrame> gpu;
    uint64_t ready_ns;
  };
  std::deque<CapturedFrame> queue_;
  uint64_t last_ready_ns_ = 0;
  std::thread worker_;
  std::exception_ptr error_;
  bool done_ = false, stopped_ = false, live_ = false;
  bool sampling_reported_ = false;
  double source_fps_ = 30, target_fps_ = 0;
  uint64_t read_frames_ = 0, kept_frames_ = 0;
  std::optional<FrameRateSampler> sampler_;
  DeviceBuffer frame_device_;
  bool device_resident_ = false;
  uint64_t fp16_d2h_bytes_ = 0;
  std::unordered_map<codec::TensorData*, std::shared_ptr<DeviceBuffer>>
      input_slots_;
  cudaStream_t preprocess_stream_ = nullptr;
  thor::TimingEvents timing_events_;
  explicit Impl(const std::filesystem::path& input,
                const std::string& decoder_backend) {
    live_ = input.string().rfind("rtsp://", 0) == 0 ||
            input.string().rfind("rtsps://", 0) == 0;
    Check(decoder_backend == "nvdec", "Thor RTSP input requires NVDEC");
    nvdec_ = std::make_unique<NvdecReader>(input);
  }
  void Start() {
    worker_ = std::thread([this] {
      try {
        for (;;) {
          std::shared_ptr<NvdecFrame> gpu;
          {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_) break;
          }
          const auto grab_begin = thor::TimingClock::now();
          const bool grabbed = nvdec_->Read(&gpu);
          const auto grab_end = thor::TimingClock::now();
          if (!grabbed) {
            Check(!live_, "RTSP input disconnected or read timed out");
            break;
          }
          const uint64_t source_index = read_frames_++;
          if (sampler_ && !sampler_->Keep(source_index)) continue;
          thor::RecordTiming("input.nvdec_decode_wall",
                             thor::Milliseconds(grab_begin, grab_end));
          ++kept_frames_;
          const auto ready_ns = static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count());
          std::unique_lock<std::mutex> lock(mutex_);
          const auto queue_begin = thor::TimingClock::now();
          space_.wait(lock, [this] { return stopped_ || queue_.size() < 4; });
          if (stopped_) break;
          thor::RecordTiming(
              "input.capture_queue_wait",
              thor::Milliseconds(queue_begin, thor::TimingClock::now()));
          queue_.push_back(CapturedFrame{std::move(gpu), ready_ns});
          available_.notify_one();
        }
      } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = std::current_exception();
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        done_ = true;
      }
      available_.notify_all();
    });
  }
  bool Read(CapturedFrame* frame) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopped_) return false;
    }
    if (!worker_.joinable()) Start();
    std::unique_lock<std::mutex> lock(mutex_);
    const auto wait_begin = thor::TimingClock::now();
    available_.wait(lock,
                    [this] { return stopped_ || done_ || !queue_.empty(); });
    if (error_) std::rethrow_exception(error_);
    if (stopped_ || queue_.empty()) return false;
    thor::RecordTiming(
        "input.consumer_queue_wait",
        thor::Milliseconds(wait_begin, thor::TimingClock::now()));
    last_ready_ns_ = queue_.front().ready_ns;
    *frame = std::move(queue_.front());
    queue_.pop_front();
    space_.notify_one();
    return true;
  }
  void Close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
    }
    if (nvdec_) nvdec_->Cancel();
    available_.notify_all();
    space_.notify_all();
    if (worker_.joinable()) worker_.join();
    queue_.clear();
    nvdec_.reset();
    if (read_frames_ && !sampling_reported_) {
      sampling_reported_ = true;
      std::cout << "input_sampling source_fps=" << source_fps_
                << " target_fps=" << target_fps_ << " stream_fps="
                << (sampler_ ? sampler_->output_fps() : source_fps_)
                << " read_frames=" << read_frames_
                << " selected_frames=" << kept_frames_
                << " dropped_frames=" << (read_frames_ - kept_frames_) << "\n";
      std::cout << "input_fp16_d2h_bytes=" << fp16_d2h_bytes_ << "\n";
    }
  }
  ~Impl() {
    try {
      Close();
    } catch (...) {
    }
    if (preprocess_stream_) cudaStreamDestroy(preprocess_stream_);
  }
};
VideoFrameReader::VideoFrameReader(const std::filesystem::path& input)
    : VideoFrameReader(input, "nvdec") {}
VideoFrameReader::VideoFrameReader(const std::filesystem::path& input,
                                   const std::string& backend)
    : impl_(std::make_unique<Impl>(input, backend)) {
  info_ = impl_->nvdec_->info();
  if (!std::isfinite(info_.fps) || info_.fps <= 0) info_.fps = 30;
  Check(info_.width > 0 && info_.height > 0,
        "Input dimensions are unavailable");
  impl_->source_fps_ = info_.fps;
}
VideoFrameReader::VideoFrameReader(const std::filesystem::path&,
                                   const CameraCaptureOptions&, aclrtContext) {
  Check(false, "Thor camera input was removed; publish the camera as RTSP");
}
void VideoFrameReader::SetTargetFps(double target_fps) {
  Check(!impl_->worker_.joinable() && !impl_->stopped_,
        "Configure target_fps before reading input");
  impl_->sampler_.emplace(impl_->source_fps_, target_fps);
  impl_->target_fps_ = target_fps;
  info_.fps = impl_->sampler_->output_fps();
}
void VideoFrameReader::SetDeviceResident(bool enabled) {
  Check(!impl_->worker_.joinable(), "Configure GPU input before reading");
  impl_->device_resident_ = enabled;
}
uint64_t VideoFrameReader::last_source_ready_ns() const {
  return impl_->last_ready_ns_;
}
VideoFrameReader::~VideoFrameReader() = default;
void VideoFrameReader::Close() {
  if (impl_) impl_->Close();
}
bool VideoFrameReader::ReadFrameAsTensor(const TensorSpec& spec,
                                         codec::TensorData* tensor,
                                         std::vector<uint8_t>* motion_nv12) {
  Impl::CapturedFrame captured;
  if (!impl_->Read(&captured)) return false;
  Check(captured.gpu != nullptr, "NVDEC CUDA frame is required");
  const int width = captured.gpu->width;
  const int height = captured.gpu->height;
  Check(spec.dtype == DataType::kFloat16 && spec.shape.size() == 4 &&
            spec.shape[1] == 3,
        "Thor input requires NCHW FP16");
  Check(width <= spec.shape[3] && height <= spec.shape[2],
        "Input exceeds model resolution");
  const size_t frame_bytes =
      TensorShape(spec.shape).NumElements() * ElementSize(spec.dtype);
  void* destination = nullptr;
  if (impl_->device_resident_) {
    auto& slot = impl_->input_slots_[tensor];
    if (!slot || slot->bytes() != frame_bytes)
      slot = std::make_shared<DeviceBuffer>(frame_bytes);
    tensor->shape = TensorShape(spec.shape);
    tensor->dtype = spec.dtype;
    tensor->bytes.clear();
    tensor->AttachExternalBuffer(slot->data(), frame_bytes,
                                 MemoryLocation::kAcl, slot);
    destination = slot->data();
  } else {
    if (tensor->shape.dims() != spec.shape || tensor->dtype != spec.dtype ||
        tensor->bytes.size() != frame_bytes)
      *tensor = codec::MakeTensor(spec.shape, spec.dtype);
    if (impl_->frame_device_.bytes() != frame_bytes)
      impl_->frame_device_.Allocate(frame_bytes);
    destination = impl_->frame_device_.data();
  }
  if (!impl_->preprocess_stream_)
    CheckAcl(cudaStreamCreateWithFlags(&impl_->preprocess_stream_,
                                       cudaStreamNonBlocking),
             "create preprocessing stream");
  impl_->timing_events_.Record(0, impl_->preprocess_stream_);
  CheckAcl(
      cudaStreamWaitEvent(impl_->preprocess_stream_, captured.gpu->ready, 0),
      "wait for NVDEC surface");
  impl_->timing_events_.Record(1, impl_->preprocess_stream_);
  Cuda420ToFp16Yuv444(*captured.gpu, spec.shape[3], spec.shape[2], destination,
                      impl_->preprocess_stream_);
  impl_->timing_events_.Record(2, impl_->preprocess_stream_);
  if (!impl_->device_resident_) {
    CheckAcl(cudaMemcpyAsync(tensor->bytes.data(), destination, frame_bytes,
                             cudaMemcpyDeviceToHost, impl_->preprocess_stream_),
             "download requested NVDEC FP16 tensor");
    impl_->fp16_d2h_bytes_ += frame_bytes;
  }
  CheckAcl(cudaStreamSynchronize(impl_->preprocess_stream_),
           "complete NVDEC preprocessing");
  TraceNvdecInput(*captured.gpu, destination, frame_bytes,
                  impl_->preprocess_stream_);
  impl_->timing_events_.Sample("input.nvdec_surface_wait_stream", 0, 1);
  impl_->timing_events_.Sample("input.nvdec_to_fp16_stream", 1, 2);
  if (motion_nv12)
    ConvertFp16Yuv444ToNv12(*tensor, {width, height, width, height},
                            motion_nv12);
  return true;
}
DecodedVideoWriter::DecodedVideoWriter(const std::filesystem::path& output_path,
                                       const std::string& output_format,
                                       double fps, int width, int height,
                                       int crf, const std::string& bitrate,
                                       const std::string& preset)
    : output_path_(output_path),
      output_format_(GuessDecodeOutputFormat(output_path, output_format)),
      fps_(fps),
      width_(width),
      height_(height),
      crf_(crf),
      bitrate_(bitrate),
      preset_(preset) {
  if (output_format_ == "png" || output_format_ == "fp16") {
    std::filesystem::create_directories(output_path_);
    return;
  }
  Check(output_format_ == "mp4",
        "decoded video output format must be mp4 or png");
  if (!output_path_.parent_path().empty()) {
    std::filesystem::create_directories(output_path_.parent_path());
  }
  const std::string command = BuildFfmpegCommand(
      output_path_, fps_, width_, height_, crf_, bitrate_, preset_);
  pipe_ = popen(command.c_str(), "w");
  Check(pipe_ != nullptr,
        "failed to open ffmpeg pipe for decoded video output");
}

DecodedVideoWriter::~DecodedVideoWriter() {
  if (pipe_ != nullptr) {
    const int status = pclose(pipe_);
    pipe_ = nullptr;
    (void)status;
  }
}

void DecodedVideoWriter::WriteTensorFrame(const codec::TensorData& tensor,
                                          int frame_index) {
  if (output_format_ == "fp16") {
    if (const char* value = std::getenv("MLVC_FP16_SAVE_STRIDE")) {
      const int stride = std::stoi(value);
      Check(stride > 0, "FP16 save stride must be positive");
      if (frame_index % stride != 0) {
        ++frame_count_;
        return;
      }
    }
    const auto path =
        output_path_ / ("frame_" + std::to_string(frame_index) + ".fp16");
    std::ofstream output(path, std::ios::binary);
    const auto view = tensor.View();
    output.write(static_cast<const char*>(view.data()), view.bytes());
    Check(output.good(), "Cannot write reconstructed FP16 frame");
    ++frame_count_;
    return;
  }
  const cv::Mat frame = TensorToBgrMat(tensor, width_, height_);
  if (output_format_ == "png") {
    std::ostringstream name;
    name << "im" << std::setfill('0') << std::setw(5) << (frame_index + 1)
         << ".png";
    const std::filesystem::path path = output_path_ / name.str();
    Check(cv::imwrite(path.string(), frame),
          "failed to write PNG frame: " + path.string());
  } else {
    const std::size_t bytes = frame.total() * frame.elemSize();
    const std::size_t written = fwrite(frame.data, 1, bytes, pipe_);
    Check(written == bytes, "failed to write decoded frame to ffmpeg pipe");
  }
  ++frame_count_;
}

void DecodedVideoWriter::Close() {
  if (pipe_ == nullptr) {
    return;
  }
  const int status = pclose(pipe_);
  pipe_ = nullptr;
  Check(status == 0, "ffmpeg failed while writing decoded output video");
}

RtspVideoPublisher::RtspVideoPublisher(const std::string& url, double fps,
                                       int width, int height,
                                       const std::string& preset, int crf,
                                       std::size_t queue_capacity,
                                       const std::string& transport,
                                       bool h264_copy)
    : width_(width),
      height_(height),
      queue_capacity_(queue_capacity),
      h264_copy_(h264_copy) {
  Check(queue_capacity_ > 0, "RTSP output queue capacity must be positive");
  direct_state_ = std::make_unique<DirectState>();
  direct_state_->h264_copy = h264_copy_;
  direct_state_->transport = transport;
  direct_state_->input_time_base = FpsRational(fps);
  direct_state_->stream_time_base = AVRational{1, 90000};
  CheckAv(avformat_alloc_output_context2(&direct_state_->format, nullptr,
                                         "rtsp", url.c_str()),
          "allocate RTSP output context");
  Check(direct_state_->format != nullptr,
        "allocate RTSP output context returned null");
  direct_state_->stream = avformat_new_stream(direct_state_->format, nullptr);
  Check(direct_state_->stream != nullptr, "create RTSP video stream");
  direct_state_->stream->time_base = direct_state_->stream_time_base;
  direct_state_->stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
  direct_state_->stream->codecpar->codec_id = AV_CODEC_ID_H264;
  direct_state_->stream->codecpar->width = width_;
  direct_state_->stream->codecpar->height = height_;
  direct_state_->stream->codecpar->format = AV_PIX_FMT_YUV420P;

  if (!h264_copy_) {
    const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
    Check(codec != nullptr, "find an H.264 encoder for direct RTSP publishing");
    direct_state_->encoder = avcodec_alloc_context3(codec);
    Check(direct_state_->encoder != nullptr,
          "allocate direct RTSP H.264 encoder");
    direct_state_->encoder->width = width_;
    direct_state_->encoder->height = height_;
    direct_state_->encoder->pix_fmt = AV_PIX_FMT_NV12;
    direct_state_->encoder->time_base = direct_state_->input_time_base;
    direct_state_->encoder->framerate = AVRational{
        direct_state_->input_time_base.den, direct_state_->input_time_base.num};
    direct_state_->encoder->gop_size =
        std::max(1, static_cast<int>(std::lround(fps * 2.0)));
    direct_state_->encoder->max_b_frames = 0;
    // RTSP/AVC requires SPS/PPS in codecpar extradata (the SDP). Ask the
    // encoder to keep global headers separate from the packet payload.
    direct_state_->encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (direct_state_->encoder->priv_data != nullptr) {
      av_opt_set(direct_state_->encoder->priv_data, "preset", "p1", 0);
      av_opt_set(direct_state_->encoder->priv_data, "tune", "ull", 0);
      av_opt_set_int(direct_state_->encoder->priv_data, "qp", crf, 0);
    }
    CheckAv(avcodec_open2(direct_state_->encoder, codec, nullptr),
            "open direct RTSP H.264 encoder");
    CheckAv(avcodec_parameters_from_context(direct_state_->stream->codecpar,
                                            direct_state_->encoder),
            "copy direct RTSP encoder parameters");
    direct_state_->stream->time_base = direct_state_->stream_time_base;
    direct_state_->header_written = false;
  }

  if (!h264_copy_) {
    AVDictionary* options = nullptr;
    av_dict_set(&options, "rtsp_transport", transport.c_str(), 0);
    CheckAv(avformat_write_header(direct_state_->format, &options),
            "write RTSP stream header");
    av_dict_free(&options);
    direct_state_->header_written = true;
  } else {
    // Wait for the first H.264 access unit so SPS/PPS can be copied into the
    // SDP extradata before the RTSP header is sent.
    direct_state_->header_written = false;
  }
  std::cerr << "rtsp_publisher_backend=libavformat" << std::endl;
  worker_ = std::thread([this] {
    try {
      std::vector<uint8_t> reusable;
      const Nv12Layout layout{width_, height_, width_, height_};
      for (;;) {
        QueuedFrame queued;
        {
          std::unique_lock<std::mutex> lock(mutex_);
          cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
          if (queue_.empty() && stopping_) break;
          queued = std::move(queue_.front());
          queue_.pop_front();
          cv_.notify_all();
        }
        if (!queued.h264.empty()) {
          Check(direct_state_ != nullptr,
                "direct RTSP publisher state is missing");
          if (!direct_state_->header_written) {
            // Do not send the RTSP header until an access unit containing SPS
            // and PPS is available. Some VENC firmware emits those records
            // one frame after the first IDR.
            const std::vector<uint8_t> extradata =
                BuildAvccExtradata(queued.h264);
            if (extradata.empty()) continue;
            direct_state_->stream->codecpar->extradata = static_cast<uint8_t*>(
                av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            Check(direct_state_->stream->codecpar->extradata != nullptr,
                  "allocate RTSP H.264 extradata");
            std::memcpy(direct_state_->stream->codecpar->extradata,
                        extradata.data(), extradata.size());
            direct_state_->stream->codecpar->extradata_size =
                static_cast<int>(extradata.size());
            AVDictionary* options = nullptr;
            CheckAv(av_dict_set(&options, "rtsp_transport",
                                direct_state_->transport.c_str(), 0),
                    "set direct RTSP transport");
            CheckAv(avformat_write_header(direct_state_->format, &options),
                    "write RTSP stream header");
            av_dict_free(&options);
            direct_state_->header_written = true;
          }
          const std::vector<uint8_t> avcc_payload =
              ConvertAnnexBToAvcc(queued.h264);
          AVPacket* packet = av_packet_alloc();
          Check(packet != nullptr, "allocate direct RTSP H.264 packet");
          CheckAv(av_new_packet(packet, static_cast<int>(avcc_payload.size())),
                  "allocate direct RTSP H.264 payload");
          std::memcpy(packet->data, avcc_payload.data(), avcc_payload.size());
          packet->stream_index = direct_state_->stream->index;
          packet->pts = packet->dts = av_rescale_q(
              direct_state_->next_frame_pts++, direct_state_->input_time_base,
              direct_state_->stream_time_base);
          packet->duration = av_rescale_q(1, direct_state_->input_time_base,
                                          direct_state_->stream_time_base);
          CheckAv(av_interleaved_write_frame(direct_state_->format, packet),
                  "write direct RTSP H.264 packet");
          av_packet_free(&packet);
        } else if (queued.nv12.empty()) {
          Check(direct_state_ != nullptr && direct_state_->encoder != nullptr,
                "direct RTSP encoder is missing");
          ConvertFp16Yuv444ToNv12(queued.tensor, layout, &reusable);
          AVFrame* frame = av_frame_alloc();
          Check(frame != nullptr, "allocate direct RTSP NV12 frame");
          frame->format = AV_PIX_FMT_NV12;
          frame->width = width_;
          frame->height = height_;
          frame->pts = direct_state_->next_frame_pts++;
          CheckAv(av_image_fill_arrays(frame->data, frame->linesize,
                                       reusable.data(), AV_PIX_FMT_NV12, width_,
                                       height_, 1),
                  "map direct RTSP NV12 frame");
          CheckAv(avcodec_send_frame(direct_state_->encoder, frame),
                  "send direct RTSP NV12 frame");
          av_frame_free(&frame);
          for (;;) {
            AVPacket* packet = av_packet_alloc();
            Check(packet != nullptr, "allocate direct RTSP encoded packet");
            const int receive =
                avcodec_receive_packet(direct_state_->encoder, packet);
            if (receive == AVERROR(EAGAIN) || receive == AVERROR_EOF) {
              av_packet_free(&packet);
              break;
            }
            CheckAv(receive, "receive direct RTSP encoded packet");
            av_packet_rescale_ts(packet, direct_state_->encoder->time_base,
                                 direct_state_->stream->time_base);
            if (packet->pts == AV_NOPTS_VALUE)
              packet->pts = direct_state_->next_frame_pts;
            if (packet->dts == AV_NOPTS_VALUE) packet->dts = packet->pts;
            if (packet->duration <= 0) {
              packet->duration =
                  av_rescale_q(1, direct_state_->encoder->time_base,
                               direct_state_->stream->time_base);
            }
            packet->stream_index = direct_state_->stream->index;
            // Keep NVENC Annex-B framing for FFmpeg RTSP/RTP packetization.
            CheckAv(av_interleaved_write_frame(direct_state_->format, packet),
                    "write direct RTSP encoded packet");
            if (direct_state_->format->pb != nullptr)
              avio_flush(direct_state_->format->pb);
            av_packet_free(&packet);
          }
        } else {
          Check(direct_state_ != nullptr && direct_state_->encoder != nullptr,
                "direct RTSP encoder is missing");
          AVFrame* frame = av_frame_alloc();
          Check(frame != nullptr, "allocate direct RTSP NV12 frame");
          frame->format = AV_PIX_FMT_NV12;
          frame->width = width_;
          frame->height = height_;
          frame->pts = direct_state_->next_frame_pts++;
          Check(queued.nv12.size() ==
                    static_cast<std::size_t>(width_) * height_ * 3 / 2,
                "direct RTSP NV12 frame has unexpected size");
          CheckAv(av_image_fill_arrays(frame->data, frame->linesize,
                                       queued.nv12.data(), AV_PIX_FMT_NV12,
                                       width_, height_, 1),
                  "map direct RTSP NV12 frame");
          CheckAv(avcodec_send_frame(direct_state_->encoder, frame),
                  "send direct RTSP NV12 frame");
          av_frame_free(&frame);
          for (;;) {
            AVPacket* packet = av_packet_alloc();
            Check(packet != nullptr, "allocate direct RTSP encoded packet");
            const int receive =
                avcodec_receive_packet(direct_state_->encoder, packet);
            if (receive == AVERROR(EAGAIN) || receive == AVERROR_EOF) {
              av_packet_free(&packet);
              break;
            }
            CheckAv(receive, "receive direct RTSP encoded packet");
            av_packet_rescale_ts(packet, direct_state_->encoder->time_base,
                                 direct_state_->stream->time_base);
            if (packet->pts == AV_NOPTS_VALUE)
              packet->pts = direct_state_->next_frame_pts;
            if (packet->dts == AV_NOPTS_VALUE) packet->dts = packet->pts;
            if (packet->duration <= 0) {
              packet->duration =
                  av_rescale_q(1, direct_state_->encoder->time_base,
                               direct_state_->stream->time_base);
            }
            packet->stream_index = direct_state_->stream->index;
            // Keep NVENC Annex-B framing for FFmpeg RTSP/RTP packetization.
            CheckAv(av_interleaved_write_frame(direct_state_->format, packet),
                    "write direct RTSP encoded packet");
            if (direct_state_->format->pb != nullptr)
              avio_flush(direct_state_->format->pb);
            av_packet_free(&packet);
          }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        ++frame_count_;
      }
    } catch (const std::exception& error) {
      std::cerr << "rtsp_worker_error=" << error.what() << std::endl;
      std::lock_guard<std::mutex> lock(mutex_);
      worker_error_ = std::current_exception();
      stopping_ = true;
      cv_.notify_all();
    } catch (...) {
      std::cerr << "rtsp_worker_error=unknown" << std::endl;
      std::lock_guard<std::mutex> lock(mutex_);
      worker_error_ = std::current_exception();
      stopping_ = true;
      cv_.notify_all();
    }
  });
}

RtspVideoPublisher::~RtspVideoPublisher() {
  try {
    Close();
  } catch (...) {
  }
}

void RtspVideoPublisher::WriteTensorFrame(const codec::TensorData& tensor) {
  std::unique_lock<std::mutex> lock(mutex_);
  Check(direct_state_ != nullptr && !closed_, "RTSP publisher is closed");
  if (worker_error_ != nullptr) std::rethrow_exception(worker_error_);
  cv_.wait(lock, [this] {
    return stopping_ || closed_ || worker_error_ != nullptr ||
           queue_.size() < queue_capacity_;
  });
  if (worker_error_ != nullptr) std::rethrow_exception(worker_error_);
  Check(direct_state_ != nullptr && !closed_ && !stopping_,
        "RTSP publisher is closed");
  queue_.push_back(QueuedFrame{codec::CloneTensor(tensor), {}, {}});
  cv_.notify_one();
}

void RtspVideoPublisher::WriteNv12Frame(std::vector<uint8_t> frame) {
  Check(!frame.empty(), "RTSP NV12 frame must not be empty");
  std::unique_lock<std::mutex> lock(mutex_);
  Check(direct_state_ != nullptr && !closed_, "RTSP publisher is closed");
  if (worker_error_ != nullptr) std::rethrow_exception(worker_error_);
  cv_.wait(lock, [this] {
    return stopping_ || closed_ || worker_error_ != nullptr ||
           queue_.size() < queue_capacity_;
  });
  if (worker_error_ != nullptr) std::rethrow_exception(worker_error_);
  Check(direct_state_ != nullptr && !closed_ && !stopping_,
        "RTSP publisher is closed");
  queue_.push_back(QueuedFrame{{}, std::move(frame), {}});
  cv_.notify_one();
}

void RtspVideoPublisher::WriteH264Frame(std::vector<uint8_t> frame) {
  Check(h264_copy_,
        "H.264 bitstream can only be written to a copy-mode RTSP publisher");
  Check(!frame.empty(), "H.264 output frame must not be empty");
  std::unique_lock<std::mutex> lock(mutex_);
  Check(direct_state_ != nullptr && !closed_, "RTSP publisher is closed");
  cv_.wait(lock, [this] {
    return stopping_ || closed_ || worker_error_ != nullptr ||
           queue_.size() < queue_capacity_;
  });
  if (worker_error_ != nullptr) std::rethrow_exception(worker_error_);
  Check(direct_state_ != nullptr && !closed_ && !stopping_,
        "RTSP publisher is closed");
  queue_.push_back(QueuedFrame{{}, {}, std::move(frame)});
  cv_.notify_one();
}

void RtspVideoPublisher::Close() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return;
    stopping_ = true;
    closed_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
  std::exception_ptr worker_error;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    worker_error = worker_error_;
  }
  if (direct_state_ != nullptr) {
    if (direct_state_->encoder != nullptr) {
      CheckAv(avcodec_send_frame(direct_state_->encoder, nullptr),
              "flush direct RTSP H.264 encoder");
      for (;;) {
        AVPacket* packet = av_packet_alloc();
        Check(packet != nullptr, "allocate direct RTSP flush packet");
        const int receive =
            avcodec_receive_packet(direct_state_->encoder, packet);
        if (receive == AVERROR(EAGAIN) || receive == AVERROR_EOF) {
          av_packet_free(&packet);
          break;
        }
        CheckAv(receive, "receive direct RTSP flush packet");
        av_packet_rescale_ts(packet, direct_state_->encoder->time_base,
                             direct_state_->stream->time_base);
        packet->stream_index = direct_state_->stream->index;
        CheckAv(av_interleaved_write_frame(direct_state_->format, packet),
                "write direct RTSP flush packet");
        av_packet_free(&packet);
      }
    }
    if (direct_state_->header_written) {
      CheckAv(av_write_trailer(direct_state_->format), "write RTSP trailer");
    }
    direct_state_.reset();
  }
  if (worker_error != nullptr) std::rethrow_exception(worker_error);
}

bool IsVideoPath(const std::filesystem::path& path) {
  const std::string uri = path.string();
  if (uri.rfind("rtsp://", 0) == 0 || uri.rfind("rtsps://", 0) == 0)
    return true;
  const std::string extension = ToLower(path.extension().string());
  return extension == ".mp4" || extension == ".mov" || extension == ".mkv" ||
         extension == ".avi" || extension == ".webm";
}

std::string GuessDecodeOutputFormat(const std::filesystem::path& output_path,
                                    const std::string& configured_format) {
  if (!configured_format.empty()) {
    return ToLower(configured_format);
  }
  const std::string extension = ToLower(output_path.extension().string());
  if (extension == ".mp4" || extension == ".mov" || extension == ".mkv" ||
      extension == ".avi" || extension == ".webm") {
    return "mp4";
  }
  return "png";
}

void WriteTensorFrameAsPng(const codec::TensorData& tensor, int width,
                           int height, const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  const cv::Mat frame = TensorToBgrMat(tensor, width, height);
  Check(cv::imwrite(path.string(), frame),
        "failed to write PNG frame: " + path.string());
}

}  // namespace mlvc::io
