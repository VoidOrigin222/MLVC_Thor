#ifndef MLVC_PORTS_THOR_TEST_VIDEO_FIXTURE_H_
#define MLVC_PORTS_THOR_TEST_VIDEO_FIXTURE_H_

extern "C" {
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>

#include "mlvc/io/dvpp_h264_encoder.h"
#include "mlvc/runtime/acl_runtime.h"

namespace mlvc::thor {
inline void CreateHardwareTestVideo(const std::filesystem::path& file,
                                    TensorRtRuntime* runtime, int frames,
                                    int fps, int first_luma, int luma_step) {
  constexpr int kWidth = 256;
  constexpr int kHeight = 256;
  io::DvppH264EncoderConfig config;
  config.layout = {kWidth, kHeight, kWidth, kHeight};
  config.fps = fps;
  config.gop = frames;
  config.bitrate = 8'000'000;
  config.zero_copy_input = true;
  io::DvppH264Encoder encoder(runtime->context(), config);
  AVFormatContext* mux = nullptr;
  assert(avformat_alloc_output_context2(&mux, nullptr, "mp4", file.c_str()) >=
         0);
  AVStream* stream = avformat_new_stream(mux, nullptr);
  assert(stream);
  stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
  stream->codecpar->codec_id = AV_CODEC_ID_H264;
  stream->codecpar->width = kWidth;
  stream->codecpar->height = kHeight;
  stream->time_base = {1, fps};
  assert(avio_open(&mux->pb, file.c_str(), AVIO_FLAG_WRITE) >= 0);
  assert(avformat_write_header(mux, nullptr) >= 0);
  std::vector<uint8_t> nv12(kWidth * kHeight * 3 / 2, 128);
  for (int i = 0; i < frames; ++i) {
    std::fill(nv12.begin(), nv12.begin() + kWidth * kHeight,
              static_cast<uint8_t>(first_luma + i * luma_step));
    auto data = encoder.EncodeHostNv12(nv12.data(), nv12.size(), i == 0);
    AVPacket* packet = av_packet_alloc();
    assert(packet && av_new_packet(packet, static_cast<int>(data.size())) >= 0);
    std::memcpy(packet->data, data.data(), data.size());
    packet->stream_index = stream->index;
    packet->pts = packet->dts = av_rescale_q(i, {1, fps}, stream->time_base);
    packet->duration = av_rescale_q(1, {1, fps}, stream->time_base);
    if (i == 0) packet->flags |= AV_PKT_FLAG_KEY;
    assert(av_interleaved_write_frame(mux, packet) >= 0);
    av_packet_free(&packet);
  }
  assert(av_write_trailer(mux) >= 0);
  avio_closep(&mux->pb);
  avformat_free_context(mux);
}
}  // namespace mlvc::thor

#endif  // MLVC_PORTS_THOR_TEST_VIDEO_FIXTURE_H_
