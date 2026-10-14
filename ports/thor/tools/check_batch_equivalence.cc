#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "mlvc/codec/tensor_data.h"
#include "mlvc/codec/tensor_utils.h"
#include "mlvc/core/status.h"
#include "mlvc/runtime/model_manifest.h"
#include "mlvc/runtime/tensorrt_runtime.h"

namespace {
using Tensors = std::map<std::string, mlvc::codec::TensorData>;

Tensors Allocate(const std::vector<mlvc::TensorSpec>& specs) {
  Tensors result;
  for (const auto& spec : specs) {
    result.emplace(spec.name, mlvc::codec::MakeTensor(spec.shape, spec.dtype));
  }
  return result;
}

std::map<std::string, mlvc::TensorView> Views(Tensors* tensors) {
  std::map<std::string, mlvc::TensorView> result;
  for (auto& [name, tensor] : *tensors) result.emplace(name, tensor.View());
  return result;
}

void ReadFrame(const std::filesystem::path& file, mlvc::codec::TensorData* x) {
  std::ifstream input(file, std::ios::binary);
  mlvc::Check(input.good(), "Cannot open test frame: " + file.string());
  input.read(reinterpret_cast<char*>(x->bytes.data()), x->bytes.size());
  mlvc::Check(input.gcount() == static_cast<std::streamsize>(x->bytes.size()),
              "Incomplete test frame");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    mlvc::Check(argc == 4,
                "Usage: check_batch_equivalence BASE_MANIFEST BATCH_MANIFEST "
                "FP16_FRAME_DIRECTORY");
    const auto base = mlvc::ModelManifest::Load(argv[1]);
    const auto batch = mlvc::ModelManifest::Load(argv[2]);
    mlvc::VerifyRuntimeArtifacts(base);
    mlvc::VerifyRuntimeArtifacts(batch);
    std::vector<Tensors> encoder_outputs(5), decoder_outputs(5);
    for (const std::string name :
         {"MLVCEncoder", "MLVCDecoder", "MLVCReferenceFromFrame"}) {
      const auto& record = base.GetModel(name);
      std::vector<Tensors> inputs(5), expected(5), actual(5);
      for (int lane = 0; lane < 5; ++lane) {
        inputs[lane] = Allocate(record.inputs);
        expected[lane] = Allocate(record.outputs);
        actual[lane] = Allocate(record.outputs);
        if (name == "MLVCEncoder") {
          ReadFrame(std::filesystem::path(argv[3]) /
                        ("frame_" + std::to_string(16 * lane) + ".fp16"),
                    &inputs[lane].at("x"));
        } else if (name == "MLVCDecoder") {
          for (const auto* key : {"z_raw", "y_raw_0", "y_raw_1"})
            inputs[lane].at(key) = encoder_outputs[lane].at(key);
        } else {
          inputs[lane].at("ref_frame") = decoder_outputs[lane].at("x_hat");
        }
        if (inputs[lane].count("q_index_shifted")) {
          auto& tensor = inputs[lane].at("q_index_shifted");
          *reinterpret_cast<int32_t*>(tensor.bytes.data()) = 11 + lane;
        }
      }
      unsetenv("MLVC_THOR_BATCH5");
      {
        mlvc::TensorRtRuntime runtime(0);
        mlvc::TensorRtStage stage(&runtime, record,
                                  base.directory() / record.model);
        for (int lane = 0; lane < 5; ++lane)
          stage.Run(Views(&inputs[lane]), Views(&expected[lane]));
      }
      if (name == "MLVCEncoder") encoder_outputs = expected;
      if (name == "MLVCDecoder") decoder_outputs = expected;
      setenv("MLVC_THOR_BATCH5", "1", 1);
      std::vector<std::unique_ptr<mlvc::TensorRtRuntime>> runtimes;
      std::vector<std::unique_ptr<mlvc::TensorRtStage>> stages;
      std::vector<std::exception_ptr> errors(5);
      const auto& batch_record = batch.GetModel(name);
      for (int lane = 0; lane < 5; ++lane) {
        runtimes.emplace_back(std::make_unique<mlvc::TensorRtRuntime>(0));
        stages.emplace_back(std::make_unique<mlvc::TensorRtStage>(
            runtimes.back().get(), batch_record,
            batch.directory() / batch_record.model));
      }
      std::vector<std::thread> threads;
      for (int lane = 0; lane < 5; ++lane) {
        threads.emplace_back([&, lane] {
          try {
            stages[lane]->Run(Views(&inputs[lane]), Views(&actual[lane]));
          } catch (...) {
            errors[lane] = std::current_exception();
          }
        });
      }
      for (auto& thread : threads) thread.join();
      for (const auto& error : errors)
        if (error) std::rethrow_exception(error);
      for (int lane = 0; lane < 5; ++lane) {
        for (const auto& spec : record.outputs) {
          mlvc::Check(spec.dtype == mlvc::DataType::kFloat16,
                      "Equivalence probe requires FP16 outputs");
          const auto& a = actual[lane].at(spec.name);
          const auto& e = expected[lane].at(spec.name);
          const auto* av = reinterpret_cast<const uint16_t*>(a.bytes.data());
          const auto* ev = reinterpret_cast<const uint16_t*>(e.bytes.data());
          const size_t count = a.bytes.size() / sizeof(uint16_t);
          double absolute = 0, squared = 0, maximum = 0;
          uint64_t different = 0;
          for (size_t i = 0; i < count; ++i) {
            const double delta = std::abs(mlvc::codec::HalfBitsToFloat(av[i]) -
                                          mlvc::codec::HalfBitsToFloat(ev[i]));
            mlvc::Check(std::isfinite(delta), "Non-finite batch output");
            absolute += delta;
            squared += delta * delta;
            maximum = std::max(maximum, delta);
            different += av[i] != ev[i];
          }
          std::cout << "equivalence model=" << name << " lane=" << lane
                    << " tensor=" << spec.name << " mae=" << absolute / count
                    << " rmse=" << std::sqrt(squared / count)
                    << " max=" << maximum << " different=" << different
                    << " elements=" << count << '\n';
        }
      }
    }
    std::cout << "batch_equivalence=complete\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "batch_equivalence failed: " << error.what() << '\n';
    return 1;
  }
}
