#include "tensor_engine.h"

#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "mlvc/core/status.h"

namespace mlvc::thor {
namespace {
class Logger final : public nvinfer1::ILogger {
 public:
  void log(Severity severity, const char* text) noexcept override {
    if (severity <= Severity::kWARNING) {
      std::lock_guard<std::mutex> lock(mutex_);
      std::cerr << "TensorRT: " << text << '\n';
    }
  }

 private:
  std::mutex mutex_;
};
Logger logger;

std::shared_ptr<TensorEngine> Load(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  Check(file.good(), "Cannot open TensorRT engine: " + path.string());
  std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
  auto result = std::make_shared<TensorEngine>();
  result->runtime.reset(nvinfer1::createInferRuntime(logger));
  Check(result->runtime != nullptr, "Cannot create TensorRT runtime");
  result->engine.reset(
      result->runtime->deserializeCudaEngine(bytes.data(), bytes.size()));
  Check(result->engine != nullptr, "Cannot deserialize: " + path.string());
  return result;
}
}  // namespace

std::shared_ptr<TensorEngine> LoadTensorEngine(
    const std::filesystem::path& path, int device, bool share) {
  if (!share) return Load(path);
  static std::mutex mutex;
  static std::map<std::string, std::weak_ptr<TensorEngine>> cache;
  const auto key =
      std::to_string(device) + ":" + std::filesystem::canonical(path).string();
  std::lock_guard<std::mutex> lock(mutex);
  if (auto result = cache[key].lock()) return result;
  auto result = Load(path);
  cache[key] = result;
  return result;
}

}  // namespace mlvc::thor
