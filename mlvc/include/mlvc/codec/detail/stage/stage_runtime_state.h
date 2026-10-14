#ifndef MLVC_CODEC_DETAIL_STAGE_RUNTIME_STATE_H_
#define MLVC_CODEC_DETAIL_STAGE_RUNTIME_STATE_H_

#include <string_view>

#include "mlvc/codec/detail/stage/stage_types.h"
#include "mlvc/codec/detail/stage/stage_workspace.h"
#include "mlvc/core/buffer.h"
#include "mlvc/core/tensor_handle.h"
#include "mlvc/framework/codec_graph_executor.h"
#include "mlvc/framework/profiler.h"

namespace mlvc::codec {

#ifdef MLVC_TENSORRT
#define MLVC_STAGE_THREAD_LOCAL thread_local
#else
#define MLVC_STAGE_THREAD_LOCAL
#endif
// Each stream's inference thread owns its scheduler and mirror policy.
extern MLVC_STAGE_THREAD_LOCAL bool g_skip_async_entropy_cpu_mirror;
extern MLVC_STAGE_THREAD_LOCAL bool g_skip_async_encode_device_only_cpu_mirror;
extern MLVC_STAGE_THREAD_LOCAL bool g_skip_async_decode_device_only_cpu_mirror;
extern MLVC_STAGE_THREAD_LOCAL bool g_force_decode_video_output_cpu_mirror;
extern MLVC_STAGE_THREAD_LOCAL mlvc::CodecGraphExecutor* g_codec_graph_executor;
extern MLVC_STAGE_THREAD_LOCAL bool g_validate_acl_decode_prior;
extern MLVC_STAGE_THREAD_LOCAL bool g_enable_stage_fusion;
extern MLVC_STAGE_THREAD_LOCAL StageOutputWorkspace* g_stage_output_workspace;
extern MLVC_STAGE_THREAD_LOCAL void* g_acl_user_compute_stream;
#undef MLVC_STAGE_THREAD_LOCAL

class ScopedAsyncEncodeDeviceOnlyMirrorSkip {
 public:
  explicit ScopedAsyncEncodeDeviceOnlyMirrorSkip(bool enabled)
      : previous_(g_skip_async_encode_device_only_cpu_mirror) {
    if (enabled) {
      g_skip_async_encode_device_only_cpu_mirror = true;
    }
  }

  ScopedAsyncEncodeDeviceOnlyMirrorSkip(
      const ScopedAsyncEncodeDeviceOnlyMirrorSkip&) = delete;
  ScopedAsyncEncodeDeviceOnlyMirrorSkip& operator=(
      const ScopedAsyncEncodeDeviceOnlyMirrorSkip&) = delete;

  ~ScopedAsyncEncodeDeviceOnlyMirrorSkip() {
    g_skip_async_encode_device_only_cpu_mirror = previous_;
  }

 private:
  bool previous_ = false;
};

class ScopedAsyncDecodeDeviceOnlyMirrorSkip {
 public:
  explicit ScopedAsyncDecodeDeviceOnlyMirrorSkip(bool enabled)
      : previous_(g_skip_async_decode_device_only_cpu_mirror) {
    if (enabled) g_skip_async_decode_device_only_cpu_mirror = true;
  }

  ScopedAsyncDecodeDeviceOnlyMirrorSkip(
      const ScopedAsyncDecodeDeviceOnlyMirrorSkip&) = delete;
  ScopedAsyncDecodeDeviceOnlyMirrorSkip& operator=(
      const ScopedAsyncDecodeDeviceOnlyMirrorSkip&) = delete;

  ~ScopedAsyncDecodeDeviceOnlyMirrorSkip() {
    g_skip_async_decode_device_only_cpu_mirror = previous_;
  }

 private:
  bool previous_ = false;
};

class ScopedDecodeVideoOutputMirror {
 public:
  explicit ScopedDecodeVideoOutputMirror(bool enabled)
      : previous_(g_force_decode_video_output_cpu_mirror) {
    if (enabled) g_force_decode_video_output_cpu_mirror = true;
  }

  ScopedDecodeVideoOutputMirror(const ScopedDecodeVideoOutputMirror&) = delete;
  ScopedDecodeVideoOutputMirror& operator=(
      const ScopedDecodeVideoOutputMirror&) = delete;

  ~ScopedDecodeVideoOutputMirror() {
    g_force_decode_video_output_cpu_mirror = previous_;
  }

 private:
  bool previous_ = false;
};

AsyncEntropyInputs CopyEntropyInputsToPinnedFromDevice(
    std::string_view hyper_stage, std::string_view spatial_stage,
    int part_count, mlvc::PinnedHostBuffer* buffer, float force_zero_thres,
    PinnedCopySpan* copy_span, mlvc::Profiler* profiler);
bool CloneAclHandle(const mlvc::TensorHandle* source,
                    mlvc::TensorHandle* destination, std::string_view name,
                    mlvc::Profiler* profiler);
bool CloneRunOutputHandle(const RunOutput& output, std::string_view tensor_name,
                          mlvc::TensorHandle* destination,
                          std::string_view name, mlvc::Profiler* profiler);

}  // namespace mlvc::codec

#endif  // MLVC_CODEC_DETAIL_STAGE_RUNTIME_STATE_H_
