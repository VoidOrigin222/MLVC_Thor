#ifndef MLVC_CODEC_DETAIL_REFERENCE_STATE_H_
#define MLVC_CODEC_DETAIL_REFERENCE_STATE_H_

#include <optional>

#include "mlvc/codec/tensor_data.h"
#include "mlvc/core/tensor_handle.h"
#ifdef MLVC_TENSORRT
#include "mlvc/core/buffer.h"
#endif

namespace mlvc::codec {

struct ReferenceState {
#ifdef MLVC_TENSORRT
  // Persistent encoder storage survives logical GOP/reference resets.
  std::shared_ptr<mlvc::DeviceBuffer> encoder_feature_cache,
      encoder_frame_cache;
#endif
  std::optional<TensorData> frame;
  std::optional<TensorData> feature;
  std::optional<mlvc::TensorHandle> frame_handle;
  std::optional<mlvc::TensorHandle> feature_handle;

  void ResetFrame() {
    frame.reset();
    frame_handle.reset();
  }

  void ResetFeature() {
    feature.reset();
    feature_handle.reset();
  }
};

struct ReferenceFeatureBinding {
  TensorData* tensor = nullptr;
  const mlvc::TensorHandle* handle = nullptr;
};

struct SourceFrameGeometry {
  int width = 0;
  int height = 0;
  int padded_width = 0;
  int padded_height = 0;

  bool has_source_size() const { return width > 0 && height > 0; }
};

}  // namespace mlvc::codec

#endif  // MLVC_CODEC_DETAIL_REFERENCE_STATE_H_
