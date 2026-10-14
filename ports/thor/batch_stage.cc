#include "batch_stage.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "mlvc/core/status.h"
#include "tensor_engine.h"

namespace mlvc::thor {
namespace {
constexpr size_t kBatchSize = 5;

const TensorView& Find(const std::vector<NamedTensorView>& views,
                       const std::string& name) {
  for (const auto& view : views)
    if (view.name && name == view.name) return view.view;
  throw Error("Missing batch tensor: " + name);
}

nvinfer1::DataType TensorType(DataType type) {
  switch (type) {
    case DataType::kFloat16:
      return nvinfer1::DataType::kHALF;
    case DataType::kFloat32:
      return nvinfer1::DataType::kFLOAT;
    case DataType::kInt32:
      return nvinfer1::DataType::kINT32;
    case DataType::kInt8:
      return nvinfer1::DataType::kINT8;
    default:
      throw Error("Unsupported batch tensor type");
  }
}
}  // namespace

struct BatchStage::Impl {
  struct Request {
    std::vector<NamedTensorView> inputs;
    std::vector<NamedTensorView> outputs;
    std::promise<void> done;
  };
  int device;
  ModelRecord record;
  std::shared_ptr<TensorEngine> engine;
  std::unique_ptr<nvinfer1::IExecutionContext> context;
  std::map<std::string, void*> buffers;
  cudaStream_t stream = nullptr;
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<std::shared_ptr<Request>> queue;
  bool stopping = false;
  std::thread worker;
  uint64_t batches = 0;
  uint64_t full_batches = 0;
  uint64_t requests = 0;
  double inference_ms = 0;
  int wait_ms = 40;
  cudaEvent_t begin = nullptr;
  cudaEvent_t end = nullptr;

  Impl(const ModelRecord& r, const std::filesystem::path& path, int d)
      : device(d), record(r), engine(LoadTensorEngine(path, d, true)) {
    if (const char* value = std::getenv("MLVC_THOR_BATCH_WAIT_MS"))
      wait_ms = std::stoi(value);
    Check(wait_ms > 0 && wait_ms <= 1000,
          "Batch wait must be between 1 and 1000 ms");
    context.reset(engine->engine->createExecutionContext());
    Check(context != nullptr, "Cannot create batch execution context");
    Check(engine->engine->getNbIOTensors() ==
              static_cast<int>(r.inputs.size() + r.outputs.size()),
          "Batch engine I/O count mismatch");
    for (const auto* specs : {&r.inputs, &r.outputs}) {
      for (const auto& spec : *specs) {
        Check(!spec.shape.empty() && spec.shape[0] == 1,
              "Batch adapter requires a per-stream N=1 contract");
        auto expected = spec.shape;
        expected[0] = kBatchSize;
        auto shape = engine->engine->getTensorShape(spec.name.c_str());
        Check(std::vector<int64_t>(shape.d, shape.d + shape.nbDims) == expected,
              "Batch engine must have actual N=5: " + spec.name);
        Check(engine->engine->getTensorDataType(spec.name.c_str()) ==
                  TensorType(spec.dtype),
              "Batch engine dtype mismatch");
        Check(engine->engine->getTensorFormat(spec.name.c_str()) ==
                  nvinfer1::TensorFormat::kLINEAR,
              "Batch gather/scatter requires contiguous linear tensors");
        Check(engine->engine->getTensorIOMode(spec.name.c_str()) ==
                  (specs == &r.inputs ? nvinfer1::TensorIOMode::kINPUT
                                      : nvinfer1::TensorIOMode::kOUTPUT),
              "Batch engine I/O direction mismatch");
        void* buffer = nullptr;
        CheckAcl(cudaMalloc(&buffer, TensorShape(expected).NumElements() *
                                         ElementSize(spec.dtype)),
                 "allocate batch tensor");
        buffers.emplace(spec.name, buffer);
        Check(context->setTensorAddress(spec.name.c_str(), buffer),
              "Cannot bind batch tensor");
      }
    }
    CheckAcl(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking),
             "create batch stream");
    CheckAcl(cudaEventCreate(&begin), "create batch timing event");
    CheckAcl(cudaEventCreate(&end), "create batch timing event");
    worker = std::thread([this] { Work(); });
  }

  ~Impl() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
    }
    ready.notify_all();
    worker.join();
    cudaSetDevice(device);
    context.reset();
    for (const auto& item : buffers) cudaFree(item.second);
    cudaEventDestroy(begin);
    cudaEventDestroy(end);
    cudaStreamDestroy(stream);
    std::cout << "batch_stage=" << record.name
              << " actual_batch=5 batches=" << batches
              << " full_batches=" << full_batches << " requests=" << requests
              << " max_wait_ms=" << wait_ms
              << " inference_avg_ms=" << (batches ? inference_ms / batches : 0)
              << '\n';
  }

  void Execute(const std::vector<std::shared_ptr<Request>>& group) {
    // An EOF or a stalled stream cannot hold the remaining streams forever.
    // Unused slots duplicate the first request; their outputs are discarded.
    for (const auto& spec : record.inputs) {
      for (size_t slot = 0; slot < kBatchSize; ++slot) {
        const auto& view =
            Find(group[slot < group.size() ? slot : 0]->inputs, spec.name);
        auto* dst =
            static_cast<char*>(buffers.at(spec.name)) + slot * view.bytes();
        CheckAcl(cudaMemcpyAsync(dst, view.data(), view.bytes(),
                                 view.location() == MemoryLocation::kAcl
                                     ? cudaMemcpyDeviceToDevice
                                     : cudaMemcpyHostToDevice,
                                 stream),
                 "gather batch inputs");
      }
    }
    CheckAcl(cudaEventRecord(begin, stream), "start batch timer");
    Check(context->enqueueV3(stream), "TensorRT batch enqueue failed");
    CheckAcl(cudaEventRecord(end, stream), "end batch timer");
    for (const auto& spec : record.outputs) {
      for (size_t slot = 0; slot < group.size(); ++slot) {
        const auto& view = Find(group[slot]->outputs, spec.name);
        const auto* src =
            static_cast<char*>(buffers.at(spec.name)) + slot * view.bytes();
        CheckAcl(cudaMemcpyAsync(view.data(), src, view.bytes(),
                                 view.location() == MemoryLocation::kAcl
                                     ? cudaMemcpyDeviceToDevice
                                     : cudaMemcpyDeviceToHost,
                                 stream),
                 "scatter batch outputs");
      }
    }
    CheckAcl(cudaStreamSynchronize(stream), "complete batch outputs");
    float ms = 0;
    CheckAcl(cudaEventElapsedTime(&ms, begin, end), "read batch timer");
    inference_ms += ms;
    ++batches;
    full_batches += group.size() == kBatchSize;
    requests += group.size();
  }

  void Work() {
    cudaSetDevice(device);
    for (;;) {
      std::vector<std::shared_ptr<Request>> group;
      {
        std::unique_lock<std::mutex> lock(mutex);
        ready.wait(lock, [this] { return stopping || !queue.empty(); });
        if (queue.empty() && stopping) return;
        // Source probing finishes at different times. Align the first batch
        // so the five independent GOP/reset schedules begin together.
        const auto wait =
            std::chrono::milliseconds(batches == 0 ? 5000 : wait_ms);
        ready.wait_for(lock, wait, [this] {
          return stopping || queue.size() >= kBatchSize;
        });
        while (!queue.empty() && group.size() < kBatchSize) {
          group.push_back(queue.front());
          queue.pop_front();
        }
      }
      try {
        Execute(group);
        for (const auto& request : group) request->done.set_value();
      } catch (...) {
        // Requests own the input/output storage only until their futures
        // complete, including error paths with copies already enqueued.
        cudaStreamSynchronize(stream);
        for (const auto& request : group)
          request->done.set_exception(std::current_exception());
      }
    }
  }
};

BatchStage::BatchStage(const ModelRecord& record,
                       const std::filesystem::path& path, int device)
    : impl_(std::make_unique<Impl>(record, path, device)) {}
BatchStage::~BatchStage() = default;

std::shared_ptr<BatchStage> BatchStage::Get(const ModelRecord& record,
                                            const std::filesystem::path& path,
                                            int device) {
  static std::mutex mutex;
  static std::map<std::string, std::weak_ptr<BatchStage>> stages;
  const std::string key =
      std::to_string(device) + ":" + std::filesystem::canonical(path).string();
  std::lock_guard<std::mutex> lock(mutex);
  if (auto stage = stages[key].lock()) return stage;
  auto stage =
      std::shared_ptr<BatchStage>(new BatchStage(record, path, device));
  stages[key] = stage;
  return stage;
}

void BatchStage::Run(const NamedTensorView* inputs, size_t input_count,
                     const NamedTensorView* outputs, size_t output_count) {
  auto request = std::make_shared<Impl::Request>();
  request->inputs.assign(inputs, inputs + input_count);
  request->outputs.assign(outputs, outputs + output_count);
  for (const auto* specs : {&impl_->record.inputs, &impl_->record.outputs}) {
    for (const auto& spec : *specs) {
      const auto& view = Find(
          specs == &impl_->record.inputs ? request->inputs : request->outputs,
          spec.name);
      Check(view.shape().dims() == spec.shape && view.dtype() == spec.dtype &&
                view.data() != nullptr &&
                view.bytes() == TensorShape(spec.shape).NumElements() *
                                    ElementSize(spec.dtype),
            "Batch per-stream tensor contract mismatch: " + spec.name);
    }
  }
  auto done = request->done.get_future();
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Check(!impl_->stopping, "Batch worker is stopping");
    impl_->queue.push_back(request);
  }
  impl_->ready.notify_one();
  done.get();
}

}  // namespace mlvc::thor
