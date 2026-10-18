extern "C" {
#include <libavutil/frame.h>
}

#include <dlfcn.h>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "mlvc/core/buffer.h"
#include "mlvc/core/status.h"
#include "mlvc/runtime/acl_runtime.h"
#include "nvdec_reader.h"

int main(int argc, char** argv) {
  try {
    mlvc::Check(argc == 3 || argc == 4,
                "usage: probe_nvdec_surfaces VIDEO_OR_RTSP OUTPUT_CSV "
                "[MAX_FRAMES]");
    const int max_frames = argc == 4 ? std::stoi(argv[3]) : 0;
    mlvc::Check(max_frames >= 0, "MAX_FRAMES must be nonnegative");
    using Digest =
        unsigned char* (*)(const unsigned char*, size_t, unsigned char*);
    void* crypto = dlopen("libcrypto.so.3", RTLD_NOW);
    auto digest =
        crypto ? reinterpret_cast<Digest>(dlsym(crypto, "SHA256")) : nullptr;
    mlvc::Check(digest, "diagnostic SHA256 requires libcrypto.so.3");
    mlvc::TensorRtRuntime runtime(0);
    mlvc::io::NvdecReader reader(argv[1]);
    mlvc::DeviceBuffer tensor(static_cast<size_t>(3) * 1088 * 1920 * 2);
    cudaStream_t stream = nullptr;
    mlvc::CheckAcl(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking),
                   "create probe stream");
    std::ofstream out(argv[2]);
    mlvc::Check(out.good(), "open probe CSV");
    out << "frame,pts,surface_sha256,tensor_sha256\n";
    std::vector<unsigned char> raw, host(tensor.bytes());
    std::shared_ptr<mlvc::io::NvdecFrame> frame;
    int index = 0;
    auto write_sha = [&](const std::vector<unsigned char>& data) {
      unsigned char sha[32];
      digest(data.data(), data.size(), sha);
      for (auto byte : sha)
        out << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(byte);
    };
    while ((max_frames == 0 || index < max_frames) && reader.Read(&frame)) {
      const size_t pitch = frame->width * (frame->wide ? 2 : 1),
                   ybytes = pitch * frame->height;
      raw.resize(ybytes * 3 / 2);
      mlvc::CheckAcl(cudaStreamWaitEvent(stream, frame->ready, 0),
                     "wait probe surface");
      mlvc::CheckAcl(cudaMemcpy2DAsync(raw.data(), pitch, frame->luma,
                                       frame->luma_pitch, pitch, frame->height,
                                       cudaMemcpyDeviceToHost, stream),
                     "probe raw luma");
      mlvc::CheckAcl(
          cudaMemcpy2DAsync(raw.data() + ybytes, pitch, frame->chroma,
                            frame->chroma_pitch, pitch, frame->height / 2,
                            cudaMemcpyDeviceToHost, stream),
          "probe raw chroma");
      mlvc::io::Cuda420ToFp16Yuv444(*frame, 1920, 1088, tensor.data(), stream);
      mlvc::CheckAcl(cudaMemcpyAsync(host.data(), tensor.data(), host.size(),
                                     cudaMemcpyDeviceToHost, stream),
                     "probe FP16 tensor");
      mlvc::CheckAcl(cudaStreamSynchronize(stream), "complete probe downloads");
      out << std::dec << index << ',' << frame->frame->pts << ',';
      write_sha(raw);
      out << ',';
      write_sha(host);
      out << '\n';
      mlvc::Check(out.good(), "write probe CSV");
      ++index;
    }
    mlvc::CheckAcl(cudaStreamDestroy(stream), "destroy probe stream");
    dlclose(crypto);
    std::cout << "frames=" << index << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
