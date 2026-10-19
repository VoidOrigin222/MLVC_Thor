#include "mlvc/thor/multi_config.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <set>
#include <string_view>
#include <utility>

#include "mlvc/core/status.h"

namespace mlvc::thor {
namespace {

void CheckKeys(const toml::table& table,
               std::initializer_list<std::string_view> allowed) {
  for (const auto& [key, value] : table) {
    (void)value;
    Check(std::find(allowed.begin(), allowed.end(), key.str()) != allowed.end(),
          "unknown multistream config key: " + std::string(key.str()));
  }
}

toml::table Merge(toml::table base, const toml::table& overrides) {
  for (const auto& [key, value] : overrides) {
    auto* existing = base[key].as_table();
    if (existing && value.is_table()) {
      base.insert_or_assign(key, Merge(*existing, *value.as_table()));
    } else {
      base.insert_or_assign(key, value);
    }
  }
  return base;
}

void ReservePort(std::set<int64_t>& ports, const toml::table& stream,
                 const char* key, bool paired) {
  const auto port = stream[key].value<int64_t>();
  Check(port && *port > 0 && *port <= (paired ? 65534 : 65535),
        std::string("invalid multistream port: ") + key);
  Check(ports.insert(*port).second, "multistream ports overlap");
  if (paired)
    Check(ports.insert(*port + 1).second, "multistream RTP/RTCP ports overlap");
}

}  // namespace

MultiStreamConfig LoadMultiStreamConfig(const std::filesystem::path& path,
                                        const std::string& operation) {
  Check(operation == "encode" || operation == "decode",
        "Operation must be encode or decode");
  toml::table root;
  try {
    root = toml::parse_file(path.string());
  } catch (const toml::parse_error& error) {
    throw Error("Cannot parse multistream config: " +
                std::string(error.what()));
  }
  const std::string role = operation == "encode" ? "encoder" : "decoder";
  CheckKeys(root, {"execution", "stream_count", "log_prefix", "encoder",
                   "decoder", "streams"});
  const auto execution = root["execution"].value<std::string>();
  Check(execution && (*execution == "contexts" || *execution == "batch5"),
        "execution must be contexts or batch5");
  auto prefix = root["log_prefix"].value<std::string>();
  Check(!root.contains("log_prefix") || (prefix && !prefix->empty()),
        "log_prefix must be a nonempty string");
  const auto* defaults = root[role].as_table();
  Check(defaults != nullptr, "missing [" + role + "] defaults");
  const auto* streams = root["streams"].as_array();
  Check(streams && !streams->empty(),
        "multistream config requires at least one [[streams]] entry");
  size_t stream_count = streams->size();
  if (root.contains("stream_count")) {
    const auto count = root["stream_count"].value<int64_t>();
    Check(root["stream_count"].is_integer() && count && *count > 0,
          "stream_count must be a positive integer");
    Check(static_cast<uint64_t>(*count) <= streams->size(),
          "stream_count exceeds the number of configured [[streams]] entries");
    stream_count = static_cast<size_t>(*count);
  }
  Check(*execution != "batch5" || stream_count == 5,
        "batch5 requires exactly five active streams");
  MultiStreamConfig result{*execution, prefix.value_or("logs/" + role), {}};
  std::set<int64_t> ports;
  // Spare definitions are not parsed, connected, or assigned runtime resources.
  for (size_t index = 0; index < stream_count; ++index) {
    const auto& node = (*streams)[index];
    const auto* lane = node.as_table();
    Check(lane != nullptr, "each stream must be a table");
    CheckKeys(*lane, {"encoder", "decoder"});
    const auto* overrides = (*lane)[role].as_table();
    Check(overrides != nullptr, "stream requires [streams." + role + "]");
    auto stream = Merge(*defaults, *overrides);
    if (!stream.contains("mode")) stream.insert("mode", operation);
    if (operation == "encode") {
      Check(stream["output_transport_mode"].value<std::string>() == "rtp",
            "multistream encode requires output_transport_mode = rtp");
      ReservePort(ports, stream, "output_transport_port", true);
      const auto stats_port = stream["ui_stats_port"].value<int64_t>();
      if (stream.contains("ui_stats_port")) {
        Check(stats_port && *stats_port >= 0 && *stats_port <= 65535,
              "invalid ui_stats_port");
        if (*stats_port > 0) ReservePort(ports, stream, "ui_stats_port", false);
      }
    } else {
      Check(stream["input_transport_mode"].value<std::string>() == "rtp",
            "multistream decode requires input_transport_mode = rtp");
      ReservePort(ports, stream, "input_transport_port", true);
    }
    result.streams.push_back(std::move(stream));
  }
  return result;
}

}  // namespace mlvc::thor
