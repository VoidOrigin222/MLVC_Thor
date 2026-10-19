#include <unistd.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include "mlvc/core/status.h"
#include "mlvc/thor/multi_config.h"

namespace {

class ConfigFixture {
 public:
  ConfigFixture()
      : path(std::filesystem::temp_directory_path() /
             ("thor-multi-config-" + std::to_string(getpid()) + ".toml")) {}
  ~ConfigFixture() { std::filesystem::remove(path); }
  void Write(const toml::table& table) {
    std::ofstream out(path);
    out << table;
    assert(out.good());
  }
  void Reject(const toml::table& table, const std::string& expected,
              const std::string& operation = "encode") {
    Write(table);
    bool rejected = false;
    try {
      mlvc::thor::LoadMultiStreamConfig(path, operation);
    } catch (const mlvc::Error& error) {
      rejected = std::string(error.what()).find(expected) != std::string::npos;
    }
    assert(rejected);
  }
  const std::filesystem::path path;
};

}  // namespace

int main(int argc, char** argv) {
  assert(argc == 3);
  const std::filesystem::path config_path(argv[1]);
  const auto root = toml::parse_file(config_path.string());
  assert(root["stream_count"].value<int>() == 5);
  assert(root["streams"].as_array()->size() == 8);
  // Codec fields match the validated templates; UI metrics are now enabled.
  for (const std::string operation : {"encode", "decode"}) {
    const auto combined = mlvc::thor::LoadMultiStreamConfig(
        operation == "encode" ? config_path : argv[2], operation);
    assert(combined.execution == "contexts");
    assert(combined.log_prefix == "logs/" + (operation == "encode"
                                                 ? std::string("encoder")
                                                 : std::string("decoder")));
    assert(combined.streams.size() == 5);
    for (int lane = 0; lane < 5; ++lane) {
      const auto old_path = config_path.parent_path() / "five/contexts" /
                            (operation + "-" + std::to_string(lane) + ".toml");
      auto expected = toml::parse_file(old_path.string());
      if (operation == "encode") {
        // RTSP source FPS is detected; only target_fps is needed in the total
        // config.
        expected.erase("fps");
        assert(!combined.streams[lane].contains("fps"));
        expected.insert_or_assign("ui_stats_host", "192.168.5.3");
        expected.insert_or_assign("ui_stats_port", 39600 + lane);
      }
      assert(combined.streams[lane] == expected);
    }
  }

  ConfigFixture fixture;
  auto changed = root;
  auto* overrides = (*changed["streams"].as_array())[0].as_table();
  (*overrides)["encoder"].as_table()->insert_or_assign(
      "pipeline", toml::table{{"queue_capacity", 2}});
  fixture.Write(changed);
  auto combined = mlvc::thor::LoadMultiStreamConfig(fixture.path, "encode");
  assert(combined.streams[0]["pipeline"]["queue_capacity"].value<int>() == 2);
  assert(combined.streams[0]["pipeline"]["entropy_workers"].value<int>() == 1);
  assert(combined.streams[1]["pipeline"]["queue_capacity"].value<int>() == 3);

  changed = root;
  changed.insert_or_assign("execution", "batch5");
  fixture.Write(changed);
  assert(mlvc::thor::LoadMultiStreamConfig(fixture.path, "encode").execution ==
         "batch5");
  changed.insert_or_assign("execution", "typo");
  fixture.Reject(changed, "execution");
  changed = root;
  changed.insert_or_assign("unknown", 1);
  fixture.Reject(changed, "unknown multistream config key");
  changed = root;
  changed["streams"].as_array()->clear();
  fixture.Reject(changed, "at least one");
  for (const int count : {1, 2, 3, 4}) {
    changed = root;
    changed.insert_or_assign("stream_count", count);
    changed["encoder"].as_table()->insert_or_assign("fps", 30);
    changed["encoder"].as_table()->insert_or_assign("target_fps", 30);
    fixture.Write(changed);
    combined = mlvc::thor::LoadMultiStreamConfig(fixture.path, "encode");
    assert(combined.streams.size() == static_cast<size_t>(count));
    assert(combined.streams.back()["target_fps"].value<int>() == 30);
    changed.insert_or_assign("execution", "batch5");
    fixture.Reject(changed, "batch5 requires exactly five active streams");
  }
  changed = root;
  changed.insert_or_assign("stream_count", 6);
  fixture.Write(changed);
  assert(mlvc::thor::LoadMultiStreamConfig(fixture.path, "encode")
             .streams.size() == 6);
  changed = root;
  for (int lane = 0; lane < 5; ++lane) {
    auto* lane_table = (*changed["streams"].as_array())[lane].as_table();
    auto* stream = (*lane_table)["encoder"].as_table();
    stream->insert_or_assign("fps", lane < 2 ? 30 : 20);
    stream->insert_or_assign("target_fps", lane < 2 ? 30 : 20);
  }
  fixture.Write(changed);
  combined = mlvc::thor::LoadMultiStreamConfig(fixture.path, "encode");
  for (int lane = 0; lane < 5; ++lane)
    assert(combined.streams[lane]["target_fps"].value<int>() ==
           (lane < 2 ? 30 : 20));
  // The decoder follows each stream's FPS header, without a second FPS setting.
  changed = toml::parse_file(argv[2]);
  changed.insert_or_assign("stream_count", 3);
  fixture.Write(changed);
  assert(mlvc::thor::LoadMultiStreamConfig(fixture.path, "decode")
             .streams.size() == 3);

  for (const std::string operation : {"encode", "decode"}) {
    const auto base = operation == "encode" ? root : toml::parse_file(argv[2]);
    const std::string role = operation == "encode" ? "encoder" : "decoder";
    // Five/eight definitions remain in the file while the explicit count
    // changes.
    for (const auto [count, fps] : {std::pair{2, 30}, std::pair{3, 20},
                                    std::pair{5, 12}, std::pair{8, 5}}) {
      changed = base;
      changed.insert_or_assign("stream_count", count);
      if (operation == "encode")
        changed[role].as_table()->insert_or_assign("target_fps", fps);
      fixture.Write(changed);
      combined = mlvc::thor::LoadMultiStreamConfig(fixture.path, operation);
      assert(combined.streams.size() == static_cast<size_t>(count));
      if (operation == "encode")
        assert(combined.streams.back()["target_fps"].value<int>() == fps);
    }
    // Legacy configs without a count still activate every configured entry.
    changed = base;
    changed.erase("stream_count");
    fixture.Write(changed);
    assert(mlvc::thor::LoadMultiStreamConfig(fixture.path, operation)
               .streams.size() == 8);
    // Unused entries need not have usable ports and must not reserve resources.
    changed = base;
    changed.insert_or_assign("stream_count", 2);
    auto* spare = (*changed["streams"].as_array())[2].as_table();
    (*spare)[role].as_table()->insert_or_assign(operation == "encode"
                                                    ? "output_transport_port"
                                                    : "input_transport_port",
                                                0);
    fixture.Write(changed);
    assert(mlvc::thor::LoadMultiStreamConfig(fixture.path, operation)
               .streams.size() == 2);
    changed.insert_or_assign("stream_count", 3);
    fixture.Reject(changed, "invalid multistream port", operation);
  }
  for (const int count : {0, -1, 9}) {
    changed = root;
    changed.insert_or_assign("stream_count", count);
    fixture.Reject(changed, "stream_count");
  }
  changed = root;
  changed.insert_or_assign("stream_count", "2");
  fixture.Reject(changed, "stream_count");
  changed.insert_or_assign("stream_count", 2.0);
  fixture.Reject(changed, "stream_count");
  changed.insert_or_assign("stream_count", true);
  fixture.Reject(changed, "stream_count");
  changed = root;
  overrides = (*changed["streams"].as_array())[0].as_table();
  overrides->insert_or_assign("encdoe", toml::table{});
  fixture.Reject(changed, "unknown multistream config key");
  changed = root;
  overrides = (*changed["streams"].as_array())[1].as_table();
  (*overrides)["encoder"].as_table()->insert_or_assign("output_transport_port",
                                                       39481);
  fixture.Reject(changed, "overlap");
  (*overrides)["encoder"].as_table()->insert_or_assign("output_transport_port",
                                                       65535);
  fixture.Reject(changed, "invalid multistream port");
  changed = root;
  changed.insert_or_assign("log_prefix", 4);
  fixture.Reject(changed, "log_prefix");
  changed = root;
  overrides = (*changed["streams"].as_array())[1].as_table();
  (*overrides)["encoder"].as_table()->insert_or_assign("ui_stats_port", 39600);
  fixture.Reject(changed, "overlap");
}
