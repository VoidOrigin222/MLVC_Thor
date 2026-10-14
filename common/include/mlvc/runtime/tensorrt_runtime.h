#ifndef MLVC_COMMON_INCLUDE_MLVC_RUNTIME_TENSORRT_RUNTIME_H_
#define MLVC_COMMON_INCLUDE_MLVC_RUNTIME_TENSORRT_RUNTIME_H_
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "mlvc/core/tensor.h"
#include "mlvc/runtime/model_manifest.h"
#include <acl/acl.h>
namespace mlvc {
struct NamedTensorView {
  const char* name = nullptr;
  TensorView view;
};
void CheckAcl(aclError status, const std::string& operation);
void BootstrapAclEnvironment();
void ValidateAclManifestForAclRuntime(const ModelManifest& manifest);
class TensorRtRuntime {
 public:
  explicit TensorRtRuntime(int device);
  ~TensorRtRuntime();
  TensorRtRuntime(const TensorRtRuntime&) = delete;
  TensorRtRuntime& operator=(const TensorRtRuntime&) = delete;
  int device_id() const { return device_; }
  aclrtContext context() const { return const_cast<int*>(&device_); }
  aclrtStream stream() const { return stream_; }
  void MakeCurrent() const;

 private:
  int device_;
  aclrtStream stream_ = nullptr;
};
class TensorRtStage {
 public:
  TensorRtStage(TensorRtRuntime* runtime, ModelRecord record,
                std::filesystem::path path);
  ~TensorRtStage();
  const ModelRecord& record() const { return record_; }
  void Run(const std::map<std::string, TensorView>& inputs,
           const std::map<std::string, TensorView>& outputs);
  void Run(std::initializer_list<NamedTensorView> inputs,
           std::initializer_list<NamedTensorView> outputs);
  void RunNamed(const NamedTensorView* inputs, size_t input_count,
                const NamedTensorView* outputs, size_t output_count);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  TensorRtRuntime* runtime_;
  ModelRecord record_;
};
class TensorRtModelSet {
 public:
  TensorRtModelSet(TensorRtRuntime* runtime, ModelManifest manifest);
  const ModelManifest& manifest() const { return manifest_; }
  bool HasStage(const std::string& name) const;
  TensorRtStage& GetStage(const std::string& name);
  TensorRtStage& GetStage(std::string_view name);

 private:
  ModelManifest manifest_;
  std::map<std::string, std::unique_ptr<TensorRtStage>> stages_;
};
}  // namespace mlvc

#endif  // MLVC_COMMON_INCLUDE_MLVC_RUNTIME_TENSORRT_RUNTIME_H_
