#ifndef MLVC_THOR_MULTI_CONFIG_H_
#define MLVC_THOR_MULTI_CONFIG_H_

#include <filesystem>
#include <string>
#include <vector>

#include "toml++/toml.h"

namespace mlvc::thor {

struct MultiStreamConfig {
  std::string execution;
  std::string log_prefix;
  std::vector<toml::table> streams;
};

// Paths within the configuration remain relative to the runtime workspace.
MultiStreamConfig LoadMultiStreamConfig(const std::filesystem::path& path,
                                        const std::string& operation);

}  // namespace mlvc::thor

#endif  // MLVC_THOR_MULTI_CONFIG_H_
