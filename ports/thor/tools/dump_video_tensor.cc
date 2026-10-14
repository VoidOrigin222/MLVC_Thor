#include <dlfcn.h>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "mlvc/core/status.h"
#include "mlvc/io/video_io.h"
#include "mlvc/runtime/acl_runtime.h"
int main(int argc, char** argv) {
  try {
    mlvc::Check(argc == 5 || argc == 6,
                "usage: dump_video_tensor VIDEO BACKEND OUTPUT_DIR STRIDE "
                "[--hash-only]");
    bool hash_only = argc == 6;
    mlvc::Check(!hash_only || std::string(argv[5]) == "--hash-only",
                "unknown diagnostic option");
    using Digest =
        unsigned char* (*)(const unsigned char*, size_t, unsigned char*);
    void* crypto = hash_only ? dlopen("libcrypto.so.3", RTLD_NOW) : nullptr;
    Digest digest =
        crypto ? reinterpret_cast<Digest>(dlsym(crypto, "SHA256")) : nullptr;
    mlvc::Check(!hash_only || digest,
                "diagnostic SHA256 requires libcrypto.so.3");
    mlvc::TensorRtRuntime runtime(0);
    mlvc::io::VideoFrameReader reader(argv[1], argv[2]);
    reader.SetDeviceResident(true);
    reader.SetTargetFps(0);
    int stride = std::stoi(argv[4]);
    mlvc::Check(stride > 0, "stride must be positive");
    std::filesystem::path output(argv[3]);
    std::filesystem::create_directories(output);
    std::ofstream hashes;
    if (hash_only) hashes.open(output / "frame-sha256.csv");
    mlvc::TensorSpec spec;
    spec.shape = {1, 3, 1088, 1920};
    spec.dtype = mlvc::DataType::kFloat16;
    mlvc::codec::TensorData frame;
    std::vector<uint8_t> host;
    int index = 0, saved = 0;
    while (reader.ReadFrameAsTensor(spec, &frame)) {
      if (index % stride == 0) {
        host.resize(frame.ByteSize());
        mlvc::CheckAcl(cudaMemcpy(host.data(), frame.View().data(), host.size(),
                                  cudaMemcpyDeviceToHost),
                       "download requested tensor sample");
        if (hash_only) {
          unsigned char sha[32];
          digest(host.data(), host.size(), sha);
          hashes << std::dec << index << ',';
          for (auto v : sha)
            hashes << std::hex << std::setw(2) << std::setfill('0')
                   << static_cast<int>(v);
          hashes << '\n';
          mlvc::Check(hashes.good(), "write tensor digest");
        } else {
          std::ofstream file(
              output / ("frame_" + std::to_string(index) + ".fp16"),
              std::ios::binary);
          file.write(reinterpret_cast<const char*>(host.data()), host.size());
          mlvc::Check(file.good(), "write tensor sample");
        }
        ++saved;
      }
      ++index;
    }
    reader.Close();
    std::cout << "frames=" << index << " samples=" << saved << "\n";
    if (crypto) dlclose(crypto);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
