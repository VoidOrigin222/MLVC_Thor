#ifndef MLVC_PORTS_THOR_NVDEC_READER_H_
#define MLVC_PORTS_THOR_NVDEC_READER_H_
#include <cuda_runtime_api.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

#include "mlvc/io/video_io.h"
struct AVFrame;
namespace mlvc::io {
struct NvdecFrame {
  ~NvdecFrame();
  AVFrame* frame = nullptr;
  cudaEvent_t ready = nullptr;
  int device = 0;
  const uint8_t* luma = nullptr;
  const uint8_t* chroma = nullptr;
  int luma_pitch = 0, chroma_pitch = 0, width = 0, height = 0, depth = 8;
  int chroma_location = 1;
  bool wide = false, full_range = false;
};
class NvdecReader {
 public:
  explicit NvdecReader(const std::filesystem::path& path);
  ~NvdecReader();
  const VideoInfo& info() const;
  bool Read(std::shared_ptr<NvdecFrame>* frame);
  void Cancel();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
void Cuda420ToFp16Yuv444(const NvdecFrame& frame, int padded_width,
                         int padded_height, void* output, cudaStream_t stream);
// Opt-in diagnostic; downloads pixels only when MLVC_THOR_INPUT_DIGEST is set.
void TraceNvdecInput(const NvdecFrame& frame, const void* tensor, size_t bytes,
                     cudaStream_t stream);
}  // namespace mlvc::io

#endif  // MLVC_PORTS_THOR_NVDEC_READER_H_
