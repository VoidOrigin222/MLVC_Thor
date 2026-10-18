#include <unistd.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

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
  void Reject(const toml::table& table, const std::string& expected) {
    Write(table);
    bool rejected = false;
    try {
      mlvc::thor::LoadMultiStreamConfig(path, "encode");
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
  changed["streams"].as_array()->pop_back();
  fixture.Reject(changed, "exactly five");
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
