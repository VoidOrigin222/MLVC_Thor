#include <unistd.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

#include "mlvc/application/cli/decoder_app.h"
#include "mlvc/application/cli/encoder_app.h"
#include "mlvc/application/stream/encode/encode_setup.h"
#include "mlvc/core/status.h"

namespace {
void WriteConfig(const std::filesystem::path& path, const std::string& base,
                 const std::string& extra) {
  std::ofstream file(path);
  file << base << extra;
  assert(file.good());
}
template <typename Loader>
void ExpectRejected(Loader loader, const std::filesystem::path& path,
                    const std::string& expected) {
  bool rejected = false;
  try {
    loader(path);
  } catch (const mlvc::Error& error) {
    rejected = std::string(error.what()).find(expected) != std::string::npos;
  }
  assert(rejected);
}
}  // namespace

int main() {
  const auto path =
      std::filesystem::temp_directory_path() /
      ("thor-hardware-config-" + std::to_string(getpid()) + ".toml");
  auto manifest = path;
  manifest.replace_extension(".json");
  WriteConfig(
      manifest,
      "{\"runtime\":\"tensorrt\",\"dtype\":\"fp16\","
      "\"sidecar\":{\"file\":\"unused\",\"bytes\":0,\"sha256\":\"\"},"
      "\"models\":[{\"name\":\"MLVCEncoder\",\"bytes\":0,\"sha256\":\"\"},"
      "{\"name\":\"MLVCDecoder\",\"bytes\":0,\"sha256\":\"\"}]}",
      "");
  const std::string manifest_line =
      "manifest = \"" + manifest.string() + "\"\n";
  const std::string encode =
      "mode = \"encode\"\n" + manifest_line +
      "input_rtsp = \"rtsp://127.0.0.1:8554/input0\"\ntranslation_warp = true\n"
      "ltr_start_idx = 0\nltr_period = 0\nltr_qp_shift = 0\n";
  WriteConfig(path, encode, "");
  auto config = mlvc::LoadEncoderConfig(path);
  assert(config.stream.motion_backend == "nvenc");
  assert(config.stream.video_decode_backend == "nvdec");
  mlvc::codec::ValidateEncodeInput(config.stream);
  for (const std::string& value :
       {"", "video.mp4", "http://host/input", "rtsp://", "rtsp:///input",
        "rtsp://:8554/x", "rtsp://user@/x", "rtsp://host/with space"}) {
    const auto start = encode.find("input_rtsp = ");
    const auto end = encode.find('\n', start);
    auto invalid = encode;
    invalid.replace(start, end - start, "input_rtsp = \"" + value + "\"");
    WriteConfig(path, invalid, "");
    ExpectRejected(mlvc::LoadEncoderConfig, path, "requires input_rtsp");
  }
  for (const std::string& extra :
       {"input_video = \"video.mp4\"\n", "input = \"frames\"\n",
        "input_frame_dir = \"frames\"\n", "input_synthetic = true\n",
        "input_camera_device = \"/dev/video0\"\n", "camera_fps = 30\n",
        "motion_camera_nv12 = true\n", "[model]\ninput_video = \"video.mp4\"\n",
        "[model]\ninput_frame_dir = \"frames\"\n"}) {
    WriteConfig(path, encode, extra);
    ExpectRejected(mlvc::LoadEncoderConfig, path, "unknown config key");
  }
  WriteConfig(path, encode, "input_device_resident = false\n");
  ExpectRejected(mlvc::LoadEncoderConfig, path, "input_device_resident");
  auto direct = config.stream;
  direct.input_video_path = "video.mp4";
  bool rejected = false;
  try {
    mlvc::codec::ValidateEncodeInput(direct);
  } catch (const mlvc::Error&) {
    rejected = true;
  }
  assert(rejected);
  direct = config.stream;
  direct.input_synthetic = true;
  rejected = false;
  try {
    mlvc::codec::ValidateEncodeInput(direct);
  } catch (const mlvc::Error&) {
    rejected = true;
  }
  assert(rejected);
  assert(config.stream.ui_stats_port == 0);
  WriteConfig(path, encode, "ui_stats_port = 39600\n");
  ExpectRejected(mlvc::LoadEncoderConfig, path, "ui_stats_host is required");
  WriteConfig(path, encode,
              "ui_stats_host = \"127.0.0.1\"\nui_stats_port = -1\n");
  ExpectRejected(mlvc::LoadEncoderConfig, path, "ui_stats_port");
  WriteConfig(path, encode,
              "ui_stats_host = \"127.0.0.1\"\nui_stats_port = 39600\n");
  config = mlvc::LoadEncoderConfig(path);
  assert(config.stream.ui_stats_port == 39600);
  WriteConfig(path, encode, "motion_backend = \"libx264\"\n");
  ExpectRejected(mlvc::LoadEncoderConfig, path, "motion_backend");
  WriteConfig(path, encode, "motion_x264_threads = 4\n");
  ExpectRejected(mlvc::LoadEncoderConfig, path, "unknown config key");
  WriteConfig(path, encode, "video_decode_backend = \"opencv\"\n");
  ExpectRejected(mlvc::LoadEncoderConfig, path, "requires NVDEC");
  const std::string decode = "mode = \"decode\"\n" + manifest_line +
                             "input = \"video.mlvc\"\nformat = \"none\"\n";
  WriteConfig(path, decode, "output_transport_rtsp_encoder = \"nvenc\"\n");
  assert(mlvc::LoadDecoderConfig(path).stream.output_transport_rtsp_encoder ==
         "nvenc");
  WriteConfig(path, decode, "output_transport_rtsp_encoder = \"libx264\"\n");
  ExpectRejected(mlvc::LoadDecoderConfig, path, "requires NVENC");
  WriteConfig(path, decode, "input_rtp_queue_capacity = 0\n");
  ExpectRejected(mlvc::LoadDecoderConfig, path, "Invalid RTP receive limits");
  WriteConfig(path, decode,
              "input_rtp_overflow_policy = \"recover\"\n"
              "input_rtp_loss_policy = \"fail\"\n");
  ExpectRejected(mlvc::LoadDecoderConfig, path, "cannot discard");
  std::filesystem::remove(path);
  std::filesystem::remove(manifest);
}
