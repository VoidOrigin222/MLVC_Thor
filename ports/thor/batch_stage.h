#ifndef MLVC_PORTS_THOR_BATCH_STAGE_H_
#define MLVC_PORTS_THOR_BATCH_STAGE_H_

#include <filesystem>
#include <memory>

#include "mlvc/runtime/tensorrt_runtime.h"

namespace mlvc::thor {

// Logical inputs remain N=1 for each independent video stream. The worker
// gathers up to five requests into one actual N=5 TensorRT enqueue.
class BatchStage {
 public:
  static std::shared_ptr<BatchStage> Get(const ModelRecord& record,
                                         const std::filesystem::path& path,
                                         int device);
  ~BatchStage();
  void Run(const NamedTensorView* inputs, size_t input_count,
           const NamedTensorView* outputs, size_t output_count);

 private:
  BatchStage(const ModelRecord& record, const std::filesystem::path& path,
             int device);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mlvc::thor

#endif  // MLVC_PORTS_THOR_BATCH_STAGE_H_
