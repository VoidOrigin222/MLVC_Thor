#include "mlvc/runtime/tensorrt_runtime.h"

#include <NvInfer.h>

#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "batch_stage.h"
#include "mlvc/core/status.h"
#include "mlvc/runtime/thor_timing.h"
#include "tensor_engine.h"
namespace mlvc {
namespace {
DataType ConvertType(nvinfer1::DataType type) {
  switch (type) {
    case nvinfer1::DataType::kHALF:
      return DataType::kFloat16;
    case nvinfer1::DataType::kFLOAT:
      return DataType::kFloat32;
    case nvinfer1::DataType::kINT32:
      return DataType::kInt32;
    case nvinfer1::DataType::kINT8:
      return DataType::kInt8;
    default:
      throw Error("Unsupported TensorRT tensor data type");
  }
}
const TensorView& Find(const NamedTensorView* views, size_t count,
                       const std::string& name) {
  for (size_t i = 0; i < count; ++i)
    if (views[i].name && name == views[i].name) return views[i].view;
  throw Error("Missing TensorRT tensor: " + name);
}
}  // namespace
void CheckAcl(aclError code, const std::string& operation) {
  if (code != cudaSuccess)
    throw Error(operation + ": " + cudaGetErrorString(code));
}
void BootstrapAclEnvironment() {}
void ValidateAclManifestForAclRuntime(const ModelManifest& manifest) {
  Check(manifest.runtime() == "tensorrt", "Thor requires a TensorRT manifest");
  for (const auto& model : manifest.models())
    Check(model.backend == "tensorrt",
          "Mixed runtime model bundle is unsupported");
}
TensorRtRuntime::TensorRtRuntime(int device) : device_(device) {
  MakeCurrent();
  CheckAcl(aclrtCreateStream(&stream_), "create CUDA compute stream");
}
TensorRtRuntime::~TensorRtRuntime() {
  cudaSetDevice(device_);
  if (stream_) cudaStreamDestroy(stream_);
}
void TensorRtRuntime::MakeCurrent() const {
  CheckAcl(aclrtSetCurrentContext(context()), "select CUDA device");
}
struct TensorRtStage::Impl {
  std::shared_ptr<thor::TensorEngine> resource;
  std::shared_ptr<thor::BatchStage> batch;
  std::unique_ptr<nvinfer1::IExecutionContext> context;
  std::map<std::string, void*> buffers;
  uint64_t x_h2d_bytes = 0;
  bool stable_io = true;
  bool warmed = false;
  bool capture_failed = false;
  cudaGraph_t graph = nullptr;
  cudaGraphExec_t graph_exec = nullptr;
  uint64_t graph_launches = 0;
  std::string name;
  thor::TimingEvents timing_events;
  ~Impl() {
    if (graph_exec) cudaGraphExecDestroy(graph_exec);
    if (graph) cudaGraphDestroy(graph);
    if (stable_io)
      std::cout << "cuda_graph model=" << name << " launches=" << graph_launches
                << " capture_failed=" << capture_failed << '\n';
    if (buffers.count("x"))
      std::cout << "input_x_h2d_bytes=" << x_h2d_bytes << "\n";
    for (auto& entry : buffers) cudaFree(entry.second);
  }
  void Enqueue(cudaStream_t stream) {
    if (stable_io && warmed && !capture_failed && !graph_exec) {
      // TensorRT's first enqueue initializes lazy resources. Capture only the
      // inference, after pending input copies finish, with immutable bindings.
      CheckAcl(cudaStreamSynchronize(stream), "prepare CUDA graph capture");
      auto error =
          cudaStreamBeginCapture(stream, cudaStreamCaptureModeThreadLocal);
      if (error == cudaSuccess) {
        const bool queued = context->enqueueV3(stream);
        error = cudaStreamEndCapture(stream, &graph);
        if (queued && error == cudaSuccess && graph)
          error = cudaGraphInstantiate(&graph_exec, graph, 0);
        if (!queued || error != cudaSuccess || !graph_exec)
          capture_failed = true;
      } else {
        capture_failed = true;
      }
      if (capture_failed) {
        if (graph_exec) cudaGraphExecDestroy(graph_exec);
        graph_exec = nullptr;
        if (graph) cudaGraphDestroy(graph);
        graph = nullptr;
        (void)cudaGetLastError();
        std::cerr << "CUDA graph unavailable for " << name << ": "
                  << cudaGetErrorString(error) << "; using enqueueV3\n";
      }
    }
    if (graph_exec) {
      CheckAcl(cudaGraphLaunch(graph_exec, stream),
               "launch TensorRT CUDA graph");
      ++graph_launches;
    } else {
      Check(context->enqueueV3(stream), "TensorRT enqueue failed: " + name);
      warmed = true;
    }
  }
};
TensorRtStage::TensorRtStage(TensorRtRuntime* runtime, ModelRecord record,
                             std::filesystem::path path)
    : impl_(std::make_unique<Impl>()),
      runtime_(runtime),
      record_(std::move(record)) {
  runtime_->MakeCurrent();
  impl_->name = record_.name;
  const char* graph = std::getenv("MLVC_THOR_CUDA_GRAPH");
  impl_->stable_io = !graph || std::string(graph) != "0";
  const char* batch = std::getenv("MLVC_THOR_BATCH5");
  if (batch && std::string(batch) == "1") {
    impl_->stable_io = false;
    impl_->batch = thor::BatchStage::Get(record_, path, runtime_->device_id());
    return;
  }
  const char* shared = std::getenv("MLVC_THOR_SHARE_ENGINES");
  impl_->resource = thor::LoadTensorEngine(
      path, runtime_->device_id(), shared && std::string(shared) == "1");
  impl_->context.reset(impl_->resource->engine->createExecutionContext());
  Check(impl_->context != nullptr, "Cannot create execution context");
  Check(impl_->resource->engine->getNbIOTensors() ==
            static_cast<int>(record_.inputs.size() + record_.outputs.size()),
        "TensorRT I/O count mismatch");
  for (const auto* specs : {&record_.inputs, &record_.outputs})
    for (const auto& spec : *specs) {
      const auto shape =
          impl_->resource->engine->getTensorShape(spec.name.c_str());
      std::vector<int64_t> dimensions(shape.d, shape.d + shape.nbDims);
      Check(dimensions == spec.shape, "TensorRT shape mismatch: " + spec.name);
      Check(ConvertType(impl_->resource->engine->getTensorDataType(
                spec.name.c_str())) == spec.dtype,
            "TensorRT dtype mismatch: " + spec.name);
      Check(impl_->resource->engine->getTensorIOMode(spec.name.c_str()) ==
                (specs == &record_.inputs ? nvinfer1::TensorIOMode::kINPUT
                                          : nvinfer1::TensorIOMode::kOUTPUT),
            "TensorRT I/O direction mismatch");
      void* buffer = nullptr;
      TensorShape tensor_shape(spec.shape);
      CheckAcl(cudaMalloc(&buffer,
                          tensor_shape.NumElements() * ElementSize(spec.dtype)),
               "allocate inference buffer");
      impl_->buffers.emplace(spec.name, buffer);
      if (impl_->stable_io)
        Check(impl_->context->setTensorAddress(spec.name.c_str(), buffer),
              "Cannot bind CUDA graph tensor: " + spec.name);
    }
}
TensorRtStage::~TensorRtStage() {
  runtime_->MakeCurrent();
  cudaStreamSynchronize(runtime_->stream());
}
uint64_t TensorRtStage::cuda_graph_launches() const {
  return impl_->graph_launches;
}
void TensorRtStage::RunNamed(const NamedTensorView* inputs, size_t input_count,
                             const NamedTensorView* outputs,
                             size_t output_count) {
  runtime_->MakeCurrent();
  if (impl_->batch) {
    // Input producers and ready-event waits use this lane's stream. Complete
    // them before the broker reads the inputs from its independent stream.
    CheckAcl(cudaStreamSynchronize(runtime_->stream()),
             "batch input readiness");
    impl_->batch->Run(inputs, input_count, outputs, output_count);
    return;
  }
  const auto stream = runtime_->stream();
  const bool warmup = record_.name == "MLVCEncoder";
  const std::string timing_prefix = "model." + record_.name + ".";
  thor::TimingScope run_timer(timing_prefix + "runtime_wall", warmup);
  impl_->timing_events.Record(0, stream);
  const auto input_begin = thor::TimingClock::now();
  for (const auto& spec : record_.inputs) {
    const auto& view = Find(inputs, input_count, spec.name);
    Check(view.shape().dims() == spec.shape && view.dtype() == spec.dtype,
          "Input tensor contract mismatch: " + spec.name);
    Check(view.data() != nullptr &&
              view.bytes() == TensorShape(spec.shape).NumElements() *
                                  ElementSize(spec.dtype),
          "Invalid TensorRT input storage: " + spec.name);
    void* address = view.data();
    if (impl_->stable_io || view.location() != MemoryLocation::kAcl) {
      if (spec.name == "x" && view.location() != MemoryLocation::kAcl)
        impl_->x_h2d_bytes += view.bytes();
      address = impl_->buffers.at(spec.name);
      CheckAcl(cudaMemcpyAsync(address, view.data(), view.bytes(),
                               view.location() == MemoryLocation::kAcl
                                   ? cudaMemcpyDeviceToDevice
                                   : cudaMemcpyHostToDevice,
                               stream),
               "copy TensorRT input");
    }
    if (!impl_->stable_io)
      Check(impl_->context->setTensorAddress(spec.name.c_str(), address),
            "Cannot bind input: " + spec.name);
  }
  for (const auto& spec : record_.outputs) {
    const auto& view = Find(outputs, output_count, spec.name);
    Check(view.shape().dims() == spec.shape && view.dtype() == spec.dtype,
          "Output tensor contract mismatch: " + spec.name);
    Check(view.data() != nullptr &&
              view.bytes() == TensorShape(spec.shape).NumElements() *
                                  ElementSize(spec.dtype),
          "Invalid TensorRT output storage: " + spec.name);
    if (!impl_->stable_io)
      Check(impl_->context->setTensorAddress(
                spec.name.c_str(), view.location() == MemoryLocation::kAcl
                                       ? view.data()
                                       : impl_->buffers.at(spec.name)),
            "Cannot bind output: " + spec.name);
  }
  thor::RecordTiming(timing_prefix + "input_bind_upload_api_wall",
                     thor::Milliseconds(input_begin, thor::TimingClock::now()),
                     warmup);
  impl_->timing_events.Record(1, stream);
  const auto enqueue_begin = thor::TimingClock::now();
  impl_->Enqueue(stream);
  thor::RecordTiming(
      timing_prefix + "enqueue_api_wall",
      thor::Milliseconds(enqueue_begin, thor::TimingClock::now()), warmup);
  impl_->timing_events.Record(2, stream);
  for (const auto& spec : record_.outputs) {
    const auto& view = Find(outputs, output_count, spec.name);
    if (impl_->stable_io || view.location() != MemoryLocation::kAcl)
      CheckAcl(cudaMemcpyAsync(view.data(), impl_->buffers.at(spec.name),
                               view.bytes(),
                               view.location() == MemoryLocation::kAcl
                                   ? cudaMemcpyDeviceToDevice
                                   : cudaMemcpyDeviceToHost,
                               stream),
               "copy TensorRT output");
  }
  impl_->timing_events.Record(3, stream);
  const auto sync_begin = thor::TimingClock::now();
  CheckAcl(cudaStreamSynchronize(stream),
           "complete inference outputs for the synchronous tensor contract");
  thor::RecordTiming(timing_prefix + "fence_wall",
                     thor::Milliseconds(sync_begin, thor::TimingClock::now()),
                     warmup);
  impl_->timing_events.Sample(timing_prefix + "input_upload_stream", 0, 1,
                              warmup);
  impl_->timing_events.Sample(timing_prefix + "inference_stream", 1, 2, warmup);
  impl_->timing_events.Sample(timing_prefix + "output_download_stream", 2, 3,
                              warmup);
}
void TensorRtStage::Run(std::initializer_list<NamedTensorView> inputs,
                        std::initializer_list<NamedTensorView> outputs) {
  RunNamed(inputs.begin(), inputs.size(), outputs.begin(), outputs.size());
}
void TensorRtStage::Run(const std::map<std::string, TensorView>& inputs,
                        const std::map<std::string, TensorView>& outputs) {
  std::vector<NamedTensorView> in, out;
  for (const auto& x : inputs) in.push_back({x.first.c_str(), x.second});
  for (const auto& x : outputs) out.push_back({x.first.c_str(), x.second});
  RunNamed(in.data(), in.size(), out.data(), out.size());
}
TensorRtModelSet::TensorRtModelSet(TensorRtRuntime* runtime,
                                   ModelManifest manifest)
    : manifest_(std::move(manifest)) {
  ValidateAclManifestForAclRuntime(manifest_);
  for (const auto& record : manifest_.models())
    stages_.emplace(record.name,
                    std::make_unique<TensorRtStage>(
                        runtime, record, manifest_.directory() / record.model));
}
bool TensorRtModelSet::HasStage(const std::string& name) const {
  return stages_.count(name) != 0;
}
TensorRtStage& TensorRtModelSet::GetStage(const std::string& name) {
  return *stages_.at(name);
}
TensorRtStage& TensorRtModelSet::GetStage(std::string_view name) {
  return GetStage(std::string(name));
}
}  // namespace mlvc
