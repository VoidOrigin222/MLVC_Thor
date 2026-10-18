#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mlvc/application/stream/mlvc_internal.h"
#include "mlvc/codec/detail/stage/stage_runner.h"
#include "mlvc/codec/tensor_data.h"
#include "mlvc/core/buffer.h"
#include "mlvc/core/status.h"
#include "mlvc/runtime/tensorrt_runtime.h"

namespace {
using Tensors = std::map<std::string, mlvc::codec::TensorData>;
Tensors Allocate(const std::vector<mlvc::TensorSpec>& specs) {
  Tensors result;
  for (const auto& spec : specs)
    result.emplace(spec.name, mlvc::codec::MakeTensor(spec.shape, spec.dtype));
  return result;
}
std::map<std::string, mlvc::TensorView> Views(Tensors* tensors) {
  std::map<std::string, mlvc::TensorView> result;
  for (auto& [name, tensor] : *tensors) result.emplace(name, tensor.View());
  return result;
}

mlvc::codec::RunOutput RunWorkspaceStage(mlvc::StageModelSet* models,
                                         const std::string& name,
                                         const Tensors& tensors) {
  using mlvc::codec::RunStage;
  using mlvc::codec::TensorInput;
  if (name == "MLVCEncoder")
    return RunStage(
        models, name,
        {TensorInput("x", tensors.at("x")),
         TensorInput("ref_feature", tensors.at("ref_feature")),
         TensorInput("q_index_shifted", tensors.at("q_index_shifted"))});
  if (name == "MLVCDecoder")
    return RunStage(
        models, name,
        {TensorInput("z_raw", tensors.at("z_raw")),
         TensorInput("y_raw_0", tensors.at("y_raw_0")),
         TensorInput("y_raw_1", tensors.at("y_raw_1")),
         TensorInput("ref_feature", tensors.at("ref_feature")),
         TensorInput("q_index_shifted", tensors.at("q_index_shifted"))});
  return RunStage(models, name,
                  {TensorInput("ref_frame", tensors.at("ref_frame"))});
}
}  // namespace

int main(int argc, char** argv) {
  try {
    mlvc::Check(argc == 3,
                "Usage: check_cuda_graph_equivalence MANIFEST FP16_DIRECTORY");
    const auto manifest = mlvc::ModelManifest::Load(argv[1]);
    mlvc::VerifyRuntimeArtifacts(manifest);
    unsetenv("MLVC_THOR_BATCH5");
    std::vector<Tensors> encoded(4), decoded(4);
    for (const std::string name :
         {"MLVCEncoder", "MLVCDecoder", "MLVCReferenceFromFrame"}) {
      const auto& record = manifest.GetModel(name);
      std::vector<Tensors> inputs(4), expected(4), actual(4);
      for (int sample = 0; sample < 4; ++sample) {
        inputs[sample] = Allocate(record.inputs);
        expected[sample] = Allocate(record.outputs);
        actual[sample] = Allocate(record.outputs);
        if (inputs[sample].count("ref_feature"))
          mlvc::codec::FillFp16Tensor(.001f * (sample + 1),
                                      &inputs[sample].at("ref_feature"));
        if (name == "MLVCEncoder") {
          const auto path = std::filesystem::path(argv[2]) /
                            ("frame_" + std::to_string(sample * 16) + ".fp16");
          auto& x = inputs[sample].at("x");
          std::ifstream file(path, std::ios::binary);
          mlvc::Check(file.good(), "Cannot read fixture: " + path.string());
          file.read(reinterpret_cast<char*>(x.bytes.data()), x.bytes.size());
          mlvc::Check(
              file.gcount() == static_cast<std::streamsize>(x.bytes.size()),
              "Incomplete FP16 fixture");
        } else if (name == "MLVCDecoder") {
          for (const auto* key : {"z_raw", "y_raw_0", "y_raw_1"})
            inputs[sample].at(key) = encoded[sample].at(key);
        } else {
          inputs[sample].at("ref_frame") = decoded[sample].at("x_hat");
        }
        if (inputs[sample].count("q_index_shifted")) {
          const int32_t qp = 11 + sample;
          std::memcpy(inputs[sample].at("q_index_shifted").bytes.data(), &qp,
                      sizeof(qp));
        }
      }
      setenv("MLVC_THOR_CUDA_GRAPH", "0", 1);
      {
        mlvc::TensorRtRuntime runtime(0);
        mlvc::TensorRtStage stage(&runtime, record,
                                  manifest.directory() / record.model);
        // The control starts cold, so preparation cannot hide changed outputs.
        for (int sample = 0; sample < 4; ++sample)
          stage.Run(Views(&inputs[sample]), Views(&expected[sample]));
        mlvc::Check(stage.cuda_graph_launches() == 0,
                    "Graph-off control failed");
      }
      if (name == "MLVCEncoder") encoded = expected;
      if (name == "MLVCDecoder") decoded = expected;
      setenv("MLVC_THOR_CUDA_GRAPH", "1", 1);
      {
        mlvc::TensorRtRuntime runtime(0);
        mlvc::TensorRtStage stage(&runtime, record,
                                  manifest.directory() / record.model);
        stage.Prepare();
        const auto prepared_launches = stage.cuda_graph_launches();
        stage.Prepare();
        mlvc::Check(stage.cuda_graph_launches() == prepared_launches,
                    "Model preparation must be idempotent");
        mlvc::Check(prepared_launches == 2,
                    "Preparation must exercise graph capture and replay");
        for (int sample = 0; sample < 4; ++sample) {
          auto in = Views(&inputs[sample]);
          auto out = Views(&actual[sample]);
          std::map<std::string, std::unique_ptr<mlvc::DeviceBuffer>> gpu_in,
              gpu_out;
          // Change caller addresses after capture, and exercise CPU/GPU I/O.
          if (sample >= 2) {
            for (auto& [key, tensor] : inputs[sample]) {
              auto buffer =
                  std::make_unique<mlvc::DeviceBuffer>(tensor.ByteSize());
              mlvc::CheckAcl(
                  cudaMemcpy(buffer->data(), tensor.bytes.data(),
                             tensor.ByteSize(), cudaMemcpyHostToDevice),
                  "upload graph fixture");
              in[key] = mlvc::TensorView::Borrowed(buffer->data(),
                                                   &tensor.shape, tensor.dtype,
                                                   mlvc::MemoryLocation::kAcl);
              gpu_in.emplace(key, std::move(buffer));
            }
            for (auto& [key, tensor] : actual[sample]) {
              auto buffer =
                  std::make_unique<mlvc::DeviceBuffer>(tensor.ByteSize());
              out[key] = mlvc::TensorView::Borrowed(buffer->data(),
                                                    &tensor.shape, tensor.dtype,
                                                    mlvc::MemoryLocation::kAcl);
              gpu_out.emplace(key, std::move(buffer));
            }
          }
          stage.Run(in, out);
          for (auto& [key, buffer] : gpu_out) {
            auto& tensor = actual[sample].at(key);
            mlvc::CheckAcl(
                cudaMemcpy(tensor.bytes.data(), buffer->data(),
                           tensor.ByteSize(), cudaMemcpyDeviceToHost),
                "download graph fixture");
          }
          for (const auto& [key, tensor] : actual[sample])
            mlvc::Check(tensor.bytes == expected[sample].at(key).bytes,
                        "CUDA graph output mismatch: " + name + "." + key);
        }
        mlvc::Check(stage.cuda_graph_launches() == prepared_launches + 4,
                    "Expected four prepared CUDA graph launches: " + name);
      }
      std::cout << "graph_equivalence model=" << name
                << " samples=4 bitwise_equal=true\n";
      // Exercise the production workspace too: its old upload used a device
      // staging tensor, while direct input binds host data to the runtime.
      for (const char* direct : {"0", "1"}) {
        setenv("MLVC_THOR_DIRECT_HOST_INPUT", direct, 1);
        setenv("MLVC_THOR_SHARE_ENGINES", "1", 1);
        mlvc::TensorRtRuntime runtime(0);
        mlvc::StageModelSet models(&runtime, manifest);
        models.Prepare();
        mlvc::codec::StageOutputWorkspace workspace(
            &models,
            mlvc::codec::StageOutputBindingMode::kAclDeviceWithCpuMirror);
        mlvc::codec::ScopedRuntimeState state;
        mlvc::codec::ConfigureRuntimeState(&workspace, runtime, false);
        for (int sample = 0; sample < 4; ++sample) {
          auto output = RunWorkspaceStage(&models, name, inputs[sample]);
          for (const auto& spec : record.outputs)
            mlvc::Check(output.At(spec.name).bytes ==
                            expected[sample].at(spec.name).bytes,
                        "Workspace output mismatch: " + name + "." + spec.name);
        }
        std::cout << "workspace_equivalence model=" << name
                  << " direct_host=" << direct
                  << " samples=4 bitwise_equal=true\n";
      }
    }
    std::cout << "cuda_graph_equivalence=ok\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "cuda_graph_equivalence failed: " << error.what() << '\n';
    return 1;
  }
}
