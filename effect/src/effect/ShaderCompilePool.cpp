#include "ShaderCompilePool.h"

#include <system_error>
#include <utility>

namespace coral::detail {

ShaderCompilePool::~ShaderCompilePool() { stop(); }

bool ShaderCompilePool::start(std::string& error) {
  const std::lock_guard<std::mutex> lock(mMutex);
  if (mWorker.joinable()) return true;
  if (mStop) {
    error = "the shader compile pool was already stopped";
    return false;
  }
  try {
    mWorker = std::thread([this] { run(); });
  } catch (const std::system_error& exception) {
    error = std::string("the shader compile worker could not start: ") + exception.what();
    return false;
  }
  return true;
}

bool ShaderCompilePool::submit(std::function<void()> task) {
  if (!task) return false;
  {
    const std::lock_guard<std::mutex> lock(mMutex);
    if (!mWorker.joinable() || mStop) return false;
    mTasks.push_back(std::move(task));
  }
  mCondition.notify_one();
  return true;
}

void ShaderCompilePool::stop() {

  std::thread worker;
  {
    const std::lock_guard<std::mutex> lock(mMutex);
    if (!mWorker.joinable()) return;
    mStop = true;
    worker = std::move(mWorker);
  }
  mCondition.notify_all();
  worker.join();
}

void ShaderCompilePool::run() {
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mMutex);
      mCondition.wait(lock, [this] { return mStop || !mTasks.empty(); });
      if (mTasks.empty()) {
        if (mStop) return;
        continue;
      }
      task = std::move(mTasks.front());
      mTasks.pop_front();
    }
    if (task) task();
  }
}

} // namespace coral::detail
