#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace coral::detail {

/**
 * One background worker for GLSL -> SPIR-V compilation.
 *
 * Tasks may only touch `backend::compileShaderToSpirv`; no Device, command buffer or render state is
 * allowed here. A single worker bounds CPU use and avoids creating a thread per shader switch.
 */
class ShaderCompilePool {
public:
  ShaderCompilePool() = default;
  ~ShaderCompilePool();

  ShaderCompilePool(const ShaderCompilePool&) = delete;
  ShaderCompilePool& operator=(const ShaderCompilePool&) = delete;

  bool start(std::string& error);
  bool submit(std::function<void()> task);
  void stop();

  [[nodiscard]] bool running() const noexcept { return mWorker.joinable(); }

private:
  void run();

  std::thread mWorker;
  std::mutex mMutex;
  std::condition_variable mCondition;
  std::deque<std::function<void()>> mTasks;
  bool mStop = false;
};

} // namespace coral::detail
