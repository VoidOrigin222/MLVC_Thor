#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

#include "mlvc/application/cli/decoder_app.h"
#include "mlvc/application/cli/encoder_app.h"
#include "mlvc/application/stream/mlvc_stream.h"
#include "mlvc/core/status.h"

namespace {
thread_local std::ostream* lane_output = nullptr;

// Stream reports from the pipeline's owning thread go to its own log file.
// Worker diagnostics without an owner remain in the common process log.
class LaneLogBuffer final : public std::streambuf {
 public:
  explicit LaneLogBuffer(std::streambuf* fallback) : fallback_(fallback) {}

 protected:
  std::streamsize xsputn(const char* text, std::streamsize count) override {
    if (lane_output) return lane_output->rdbuf()->sputn(text, count);
    std::lock_guard<std::mutex> lock(mutex_);
    return fallback_->sputn(text, count);
  }
  int_type overflow(int_type ch) override {
    if (traits_type::eq_int_type(ch, traits_type::eof()))
      return traits_type::not_eof(ch);
    const char byte = traits_type::to_char_type(ch);
    return xsputn(&byte, 1) == 1 ? ch : traits_type::eof();
  }
  int sync() override {
    if (lane_output) return lane_output->rdbuf()->pubsync();
    std::lock_guard<std::mutex> lock(mutex_);
    return fallback_->pubsync();
  }

 private:
  std::streambuf* fallback_;
  std::mutex mutex_;
};

class StartGate {
 public:
  explicit StartGate(int lanes) : remaining_(lanes) {}
  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (--remaining_ == 0) ready_.notify_all();
    ready_.wait(lock, [this] { return remaining_ == 0; });
  }

 private:
  int remaining_;
  std::mutex mutex_;
  std::condition_variable ready_;
};
}  // namespace

int main(int argc, char** argv) {
  try {
    mlvc::Check(argc == 9,
                "Usage: mlvc_multi_thor encode|decode contexts|batch5 "
                "LOG_PREFIX CONFIG0 CONFIG1 CONFIG2 CONFIG3 CONFIG4");
    const std::string operation(argv[1]);
    const std::string mode(argv[2]);
    mlvc::Check(operation == "encode" || operation == "decode",
                "Operation must be encode or decode");
    mlvc::Check(mode == "contexts" || mode == "batch5",
                "Mode must be contexts or batch5");
    setenv("MLVC_THOR_SHARE_ENGINES", "1", 1);
    if (mode == "batch5")
      setenv("MLVC_THOR_BATCH5", "1", 1);
    else
      unsetenv("MLVC_THOR_BATCH5");
    const std::string prefix(argv[3]);
    std::vector<int> result(5, 1);
    StartGate gate(5);
    std::vector<std::thread> threads;
    LaneLogBuffer buffer(std::cout.rdbuf());
    LaneLogBuffer error_buffer(std::cerr.rdbuf());
    auto* original = std::cout.rdbuf(&buffer);
    auto* original_error = std::cerr.rdbuf(&error_buffer);
    for (int lane = 0; lane < 5; ++lane) {
      threads.emplace_back([&, lane] {
        std::ofstream log(prefix + "-lane" + std::to_string(lane) + ".log");
        lane_output = &log;
        bool entered = false;
        try {
          mlvc::Check(log.good(), "Cannot create per-stream log");
          if (operation == "encode") {
            const auto config = mlvc::LoadEncoderConfig(argv[4 + lane]);
            const auto binding =
                config.stream.input_device_resident
                    ? mlvc::codec::StageOutputBindingMode::
                          kAclDeviceWithCpuMirror
                    : mlvc::codec::StageOutputBindingMode::kCpu;
            mlvc::app::MlvcCodecRuntime runtime(config.stream.manifest_path,
                                                config.stream.device, binding);
            mlvc::Profiler profiler;
            mlvc::CodecGraphExecutor graph(
                config.stream.pipeline.graph_packet_capacity);
            mlvc::EntropyWorker entropy(config.stream.pipeline.entropy_workers);
            mlvc::codec::EncodePipelineServices services{&profiler, &graph,
                                                         &entropy, &runtime};
            entered = true;
            gate.Wait();
            result[lane] =
                mlvc::codec::RunEncodeStream(config.stream, &services);
          } else {
            const auto config = mlvc::LoadDecoderConfig(argv[4 + lane]);
            const auto binding =
                config.stream.output_format == "none"
                    ? mlvc::codec::StageOutputBindingMode::
                          kAclDeviceWithCpuMirror
                    : mlvc::codec::StageOutputBindingMode::kCpu;
            mlvc::app::MlvcCodecRuntime runtime(config.stream.manifest_path,
                                                config.stream.device, binding);
            mlvc::Profiler profiler;
            mlvc::CodecGraphExecutor graph(
                config.stream.pipeline.graph_packet_capacity);
            mlvc::EntropyWorker entropy(config.stream.pipeline.entropy_workers);
            mlvc::codec::DecodePipelineServices services{&profiler, &graph,
                                                         &entropy, &runtime};
            entered = true;
            gate.Wait();
            result[lane] =
                mlvc::codec::RunDecodeStream(config.stream, &services);
          }
        } catch (const std::exception& error) {
          if (!entered) gate.Wait();
          log << "lane_error=" << error.what() << '\n';
          result[lane] = 1;
        }
        log.flush();
        lane_output = nullptr;
      });
    }
    for (auto& thread : threads) thread.join();
    std::cout.rdbuf(original);
    std::cerr.rdbuf(original_error);
    int status = 0;
    for (int lane = 0; lane < 5; ++lane) {
      std::cout << "lane=" << lane << " exit=" << result[lane] << '\n';
      status |= result[lane];
    }
    return status;
  } catch (const std::exception& error) {
    std::cerr << "multistream failed: " << error.what() << '\n';
    return 1;
  }
}
