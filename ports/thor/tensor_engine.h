#ifndef MLVC_PORTS_THOR_TENSOR_ENGINE_H_
#define MLVC_PORTS_THOR_TENSOR_ENGINE_H_

#include <NvInfer.h>

#include <filesystem>
#include <memory>

namespace mlvc::thor {

// Destruction order keeps the runtime alive until its engine is destroyed.
struct TensorEngine {
  std::unique_ptr<nvinfer1::IRuntime> runtime;
  std::unique_ptr<nvinfer1::ICudaEngine> engine;
};

std::shared_ptr<TensorEngine> LoadTensorEngine(
    const std::filesystem::path& path, int device, bool share);

}  // namespace mlvc::thor

#endif  // MLVC_PORTS_THOR_TENSOR_ENGINE_H_
