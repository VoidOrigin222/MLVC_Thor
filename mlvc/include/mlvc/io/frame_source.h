#ifndef MLVC_IO_FRAME_SOURCE_H_
#define MLVC_IO_FRAME_SOURCE_H_

#include <filesystem>
#include <optional>

#include "mlvc/codec/tensor_data.h"
#include "mlvc/io/video_io.h"

namespace mlvc::io {

class FrameSource {
 public:
  FrameSource(
      int configured_frame_num, std::filesystem::path input_video_path,
      std::filesystem::path input_frame_dir, mlvc::TensorSpec frame_spec,
      std::optional<CameraCaptureOptions> camera_options = std::nullopt,
      aclrtContext context = nullptr, double target_fps = 0.0,
      bool device_resident = false,
      std::string video_decode_backend = mlvc::io::kDefaultVideoDecodeBackend);

  bool ReadFrame(int frame_index, codec::TensorData* frame,
                 std::vector<uint8_t>* motion_nv12 = nullptr);
  void Close();
  uint64_t last_source_ready_ns() const {
#ifdef MLVC_TENSORRT
    return video_reader_ ? video_reader_->last_source_ready_ns() : 0;
#else
    return 0;
#endif
  }
  bool has_video_input() const { return video_reader_.has_value(); }
  const std::filesystem::path& input_frame_dir() const {
    return input_frame_dir_;
  }

 private:
  int configured_frame_num_ = 0;
  std::filesystem::path input_frame_dir_;
  mlvc::TensorSpec frame_spec_;
  std::optional<VideoFrameReader> video_reader_;
};

}  // namespace mlvc::io

#endif  // MLVC_IO_FRAME_SOURCE_H_
