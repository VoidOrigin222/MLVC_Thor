#include <unistd.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

#include "mlvc/application/cli/decoder_app.h"
#include "mlvc/application/cli/encoder_app.h"
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
      "input_video = \"video.mp4\"\ntranslation_warp = true\n"
      "ltr_start_idx = 0\nltr_period = 0\nltr_qp_shift = 0\n";
  WriteConfig(path, encode, "");
  auto config = mlvc::LoadEncoderConfig(path);
  assert(config.stream.motion_backend == "nvenc");
  assert(config.stream.video_decode_backend == "nvdec");
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
