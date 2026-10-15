#include "mlvc/application/stream/encode/encode_output.h"

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <filesystem>
#include <string>

#include "mlvc/core/status.h"
#include "mlvc/framework/profile_range.h"

namespace mlvc::codec {

EncodeOutput::EncodeOutput(const EncodeStreamOptions& options,
                           const mlvc::io::MlvcBitstreamHeader& header,
                           const MlvcRateControlOptions& rate_options,
                           mlvc::Profiler* profiler)
    : options_(options),
      profiler_(profiler),
      fps_(rate_options.fps),
      rate_controller_(rate_options) {
  const bool udp_output = options_.output_transport_port > 0;
  Check(udp_output || !options_.output_bitstream_path.empty(),
        "encode requires output_bitstream_path or udp_port");
  if (udp_output) {
    Check(!options_.output_transport_host.empty(),
          "udp_host is required when udp_port is set");
    if (options_.output_transport_mode == "rtp") {
      rtp_sender_.emplace(options_.output_transport_host,
                          static_cast<uint16_t>(options_.output_transport_port),
                          options_.output_transport_pacing_rate_bps,
                          options_.output_transport_max_burst_bytes,
                          options_.output_transport_max_queue_bytes,
                          options_.output_transport_max_queue_delay_ms,
                          options_.output_transport_payload_type);
      rtp_sender_->SendHeader(header);
    } else {
      mlvc::transport::UdpSendOptions send_options;
      send_options.pacing_rate_bps = options_.output_transport_pacing_rate_bps;
      send_options.max_burst_bytes = options_.output_transport_max_burst_bytes;
      send_options.max_queue_bytes = options_.output_transport_max_queue_bytes;
      send_options.max_queue_delay_ms =
          options_.output_transport_max_queue_delay_ms;
      udp_sender_.emplace(options_.output_transport_host,
                          static_cast<uint16_t>(options_.output_transport_port),
                          send_options);
      udp_sender_->SendHeader(header);
    }
  } else {
    writer_.emplace(options_.output_bitstream_path, header);
  }
  if (options_.ui_stats_port > 0) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* address = nullptr;
    const std::string port = std::to_string(options_.ui_stats_port);
    const int resolved = getaddrinfo(options_.ui_stats_host.c_str(),
                                     port.c_str(), &hints, &address);
    Check(resolved == 0 && address != nullptr,
          "Cannot resolve UI statistics address");
    stats_socket_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    const bool connected =
        stats_socket_ >= 0 &&
        ::connect(stats_socket_, address->ai_addr, address->ai_addrlen) == 0;
    freeaddrinfo(address);
    if (!connected && stats_socket_ >= 0) {
      ::close(stats_socket_);
      stats_socket_ = -1;
    }
    Check(connected, "Cannot create UI statistics socket");
  }
}

EncodeOutput::~EncodeOutput() {
  try {
    Close();
  } catch (...) {
  }
  if (stats_socket_ >= 0) ::close(stats_socket_);
}

void EncodeOutput::PublishUiStats(bool force) {
  if (stats_socket_ < 0) return;
  const auto now = std::chrono::steady_clock::now();
  if (!force && now - stats_last_sent_ < std::chrono::milliseconds(500)) return;
  stats_last_sent_ = now;
  const std::string message =
      "{\"kind\":\"semantic_codec_stats\",\"version\":1,\"frames\":" +
      std::to_string(stats_frames_) +
      ",\"payload_bytes\":" + std::to_string(payload_bytes_) + "}";
  // Telemetry loss must never stall or fail the video pipeline.
  (void)::send(stats_socket_, message.data(), message.size(), MSG_DONTWAIT);
}

void EncodeOutput::Flush(PendingEncodedFrame pending) {
  std::vector<uint8_t> payload = pending.payload.get();
  {
    mlvc::ScopedCpuTimer timer(profiler_, "bitstream.frame_write");
    if (writer_.has_value()) {
      writer_->WriteFrame(pending.frame_index, pending.frame_type,
                          pending.q_index, pending.metadata, payload);
    }
    if (udp_sender_.has_value()) {
      mlvc::ScopedCpuTimer udp_timer(profiler_, "udp.send_enqueue");
      udp_sender_->SendFrame(pending.frame_index, pending.frame_type,
                             pending.q_index, payload);
    }
    if (rtp_sender_.has_value()) {
      mlvc::ScopedCpuTimer rtp_timer(profiler_, "rtp.send");
      rtp_sender_->SendFrame(pending.frame_index, pending.frame_type,
                             pending.q_index, pending.metadata, payload);
    }
  }
  if (pending.ready_at != std::chrono::steady_clock::time_point{})
    ready_to_output_.Add(
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - pending.ready_at)
            .count());
  payload_bytes_ += payload.size();
  ++stats_frames_;
  PublishUiStats();
  rate_controller_.Update(
      static_cast<double>(pending.frame_index) / fps_, pending.frame_type,
      pending.q_index, payload.size(),
      mlvc::io::kMlvcBitstreamFrameOverheadBytes +
          (pending.metadata.translation_warp &&
                   pending.frame_type != MlvcFrameType::kIFrame
               ? 2
               : 0));
}

void EncodeOutput::Close() {
  if (closed_) return;
  std::exception_ptr first_error;
  const auto close_one = [&first_error](auto& resource) {
    if (!resource.has_value()) return;
    try {
      resource->Close();
    } catch (...) {
      if (first_error == nullptr) first_error = std::current_exception();
    }
  };
  close_one(writer_);
  close_one(udp_sender_);
  close_one(rtp_sender_);
  PublishUiStats(true);
  closed_ = true;
  if (first_error != nullptr) std::rethrow_exception(first_error);
}

void EncodeOutput::SendEnd() {
  Check(!closed_, "encode output is closed");
  if (udp_sender_.has_value()) udp_sender_->SendEnd();
  if (rtp_sender_.has_value()) rtp_sender_->SendEnd();
}

bool EncodeOutput::ConsumeRandomAccessRequest() {
  return rtp_sender_.has_value() && rtp_sender_->ConsumeRandomAccessRequest();
}
bool EncodeOutput::PopMlvcControl(
    mlvc::transport::MlvcControlMessage* message) {
  return rtp_sender_.has_value() && rtp_sender_->PopMlvcControl(message);
}
void EncodeOutput::SendMlvcControlResponse(
    const mlvc::transport::MlvcControlMessage& request, bool accepted,
    const std::string& reason) {
  if (rtp_sender_.has_value())
    rtp_sender_->SendMlvcControlResponse(request, accepted, reason);
}

uint64_t EncodeOutput::file_bytes() const {
  if (!options_.output_bitstream_path.empty() &&
      std::filesystem::exists(options_.output_bitstream_path)) {
    return std::filesystem::file_size(options_.output_bitstream_path);
  }
  return payload_bytes_;
}

}  // namespace mlvc::codec
