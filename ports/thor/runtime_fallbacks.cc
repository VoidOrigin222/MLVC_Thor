#include "mlvc/core/status.h"
#include "mlvc/runtime/decode_prior_acl.h"
#include "mlvc/runtime/encode_index_acl.h"
#include "mlvc/runtime/fp16_yuv444_to_nv12_acl.h"
namespace mlvc {
// Preserve the audited CPU entropy/prior implementation for the first port.
// Call sites query availability before attempting a device-only operator.
bool DecodePriorAclAvailable() { return false; }
bool EncodeIndexAclAvailable() { return false; }
DecodePriorAclResult DecodePriorSinglePartAcl(const void*, DataType, int, int,
                                              int, int, int, void*, void*,
                                              Profiler*) {
  throw Error("CUDA prior operator is not enabled");
}
DecodePriorAclResult DecodePriorRestoreYAcl(const int8_t*, const void*, int,
                                            int, int, int, int, void*, void*,
                                            Profiler*) {
  throw Error("CUDA prior operator is not enabled");
}
DecodePriorAclResult DecodePriorApplyChannelQuantStepAcl(const void*, int,
                                                         const void*, int, int,
                                                         int, void*, void*,
                                                         Profiler*) {
  throw Error("CUDA prior operator is not enabled");
}
DecodePriorAclResult DecodePriorInt8ToFp16Acl(const int8_t*, const int64_t*,
                                              size_t, size_t, void*, void*,
                                              Profiler*) {
  throw Error("CUDA prior operator is not enabled");
}
EncodeIndexAclResult BuildIndexEncodeAcl(const void*, DataType, const void*,
                                         DataType, const int64_t*, size_t,
                                         size_t, float, int16_t*, uint8_t*,
                                         void*, Profiler*) {
  throw Error("CUDA entropy index operator is not enabled");
}
void PrepareAscendRuntimeEnvironment(char**) {}
}  // namespace mlvc
