#ifndef MLVC_PORTS_THOR_INCLUDE_ACL_ACL_H_
#define MLVC_PORTS_THOR_INCLUDE_ACL_ACL_H_
// Transitional device API for the existing tensor scheduler. Every operation
// uses CUDA; this target does not load or link any Ascend library.
#include <cuda_runtime_api.h>

#include <cstring>
using aclError = cudaError_t;
using aclrtStream = cudaStream_t;
using aclrtEvent = cudaEvent_t;
using aclrtContext = void*;
using aclrtMemcpyKind = cudaMemcpyKind;
constexpr auto ACL_SUCCESS = cudaSuccess;
constexpr auto ACL_ERROR_NONE = cudaSuccess;
constexpr auto ACL_MEMCPY_HOST_TO_DEVICE = cudaMemcpyHostToDevice;
constexpr auto ACL_MEMCPY_DEVICE_TO_HOST = cudaMemcpyDeviceToHost;
constexpr auto ACL_MEMCPY_DEVICE_TO_DEVICE = cudaMemcpyDeviceToDevice;
constexpr auto ACL_MEMCPY_HOST_TO_HOST = cudaMemcpyHostToHost;
constexpr int ACL_MEM_MALLOC_NORMAL_ONLY = 0;
constexpr int ACL_MEM_MALLOC_HUGE_FIRST = 0;
constexpr unsigned ACL_EVENT_SYNC = 0;
constexpr unsigned ACL_EVENT_TIME_LINE = 0;
inline thread_local aclrtContext thor_current_context = nullptr;
inline aclError aclrtSetCurrentContext(aclrtContext c) {
  if (!c) return cudaErrorInvalidValue;
  auto error = cudaSetDevice(*static_cast<int*>(c));
  if (error == cudaSuccess) thor_current_context = c;
  return error;
}
inline aclError aclrtGetCurrentContext(aclrtContext* c) {
  *c = thor_current_context;
  return cudaSuccess;
}
inline aclError aclrtCreateStream(aclrtStream* s) {
  return cudaStreamCreateWithFlags(s, cudaStreamNonBlocking);
}
inline aclError aclrtDestroyStream(aclrtStream s) {
  return cudaStreamDestroy(s);
}
inline aclError aclrtSynchronizeStream(aclrtStream s) {
  return cudaStreamSynchronize(s);
}
inline aclError aclrtMalloc(void** p, size_t n, int) {
  return cudaMalloc(p, n);
}
inline aclError aclrtFree(void* p) { return cudaFree(p); }
inline aclError aclrtMallocHost(void** p, size_t n) {
  return cudaMallocHost(p, n);
}
inline aclError aclrtFreeHost(void* p) { return cudaFreeHost(p); }
inline aclError aclrtMemcpy(void* d, size_t capacity, const void* s, size_t n,
                            aclrtMemcpyKind k) {
  if (n > capacity) return cudaErrorInvalidValue;
  return cudaMemcpy(d, s, n, k);
}
inline aclError aclrtMemcpyAsync(void* d, size_t capacity, const void* s,
                                 size_t n, aclrtMemcpyKind k,
                                 aclrtStream stream) {
  if (n > capacity) return cudaErrorInvalidValue;
  return cudaMemcpyAsync(d, s, n, k, stream);
}
inline aclError aclrtMemset(void* d, size_t capacity, int value, size_t n) {
  if (n > capacity) return cudaErrorInvalidValue;
  return cudaMemset(d, value, n);
}
inline aclError aclrtCreateEvent(aclrtEvent* e) { return cudaEventCreate(e); }
inline aclError aclrtCreateEventWithFlag(aclrtEvent* e, unsigned) {
  return cudaEventCreate(e);
}
inline aclError aclrtDestroyEvent(aclrtEvent e) { return cudaEventDestroy(e); }
inline aclError aclrtRecordEvent(aclrtEvent e, aclrtStream s) {
  return cudaEventRecord(e, s);
}
inline aclError aclrtSynchronizeEvent(aclrtEvent e) {
  return cudaEventSynchronize(e);
}
inline aclError aclrtStreamWaitEvent(aclrtStream s, aclrtEvent e) {
  return cudaStreamWaitEvent(s, e, 0);
}
inline aclError aclrtEventElapsedTime(float* ms, aclrtEvent a, aclrtEvent b) {
  return cudaEventElapsedTime(ms, a, b);
}

#endif  // MLVC_PORTS_THOR_INCLUDE_ACL_ACL_H_
