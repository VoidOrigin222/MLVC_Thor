#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>
#include <vector>

#include "mlvc/application/stream/mlvc_internal.h"
#include "mlvc/codec/detail/stage/stage_runtime_state.h"

int main() {
  std::atomic<int> entered{0};
  std::atomic<bool> release{false};
  std::vector<std::thread> threads;
  for (int lane = 0; lane < 5; ++lane) {
    threads.emplace_back([&, lane] {
      using mlvc::codec::g_enable_stage_fusion;
      using mlvc::codec::g_skip_async_entropy_cpu_mirror;
      assert(!g_enable_stage_fusion && !g_skip_async_entropy_cpu_mirror);
      {
        mlvc::codec::ScopedRuntimeState state;
        g_enable_stage_fusion = lane % 2;
        g_skip_async_entropy_cpu_mirror = !(lane % 2);
        {
          mlvc::codec::ScopedRuntimeState nested;
          g_enable_stage_fusion = !(lane % 2);
        }
        assert(g_enable_stage_fusion == static_cast<bool>(lane % 2));
        ++entered;
        while (!release) std::this_thread::yield();
        assert(g_enable_stage_fusion == static_cast<bool>(lane % 2));
        assert(g_skip_async_entropy_cpu_mirror == !(lane % 2));
      }
      assert(!g_enable_stage_fusion && !g_skip_async_entropy_cpu_mirror);
    });
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (entered != 5 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const bool concurrent = entered == 5;
  release = true;
  for (auto& thread : threads) thread.join();
  assert(concurrent);
  assert(!mlvc::codec::g_enable_stage_fusion);
  assert(!mlvc::codec::g_skip_async_entropy_cpu_mirror);
}
