/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <glog/logging.h>

#include <folly/Portability.h>
#include <folly/experimental/observer/detail/Core.h>
#include <folly/experimental/observer/detail/GraphCycleDetector.h>
#include <folly/fibers/FiberManager.h>
#include <folly/functional/Invoke.h>
#include <folly/futures/Future.h>
#include <folly/synchronization/SanitizeThread.h>

namespace folly {
namespace observer_detail {

/**
 * ObserverManager is a singleton which controls the re-computation of all
 * Observers. Such re-computation always happens on the thread pool owned by
 * ObserverManager.
 *
 * ObserverManager has global current version. All existing Observers
 * may have their version be less (yet to be updated) or equal (up to date)
 * to the global current version.
 *
 * ObserverManager::CurrentQueue contains all of the Observers which need to be
 * updated to the global current version. Those updates are peformed on the
 * ObserverManager's thread pool, until the queue is empty. If some Observer is
 * updated, all of its dependents are added to ObserverManager::CurrentQueue
 * to be updated.
 *
 * If some leaf Observer (i.e. created from Observable) is updated, then current
 * version of the ObserverManager should be bumped. All such updated leaf
 * Observers are added to the ObserverManager::NextQueue.
 *
 * *Only* when ObserverManager::CurrentQueue is empty, the global current
 * version is bumped and all updates from the ObserverManager::NextQueue are
 * performed. If leaf Observer gets updated more then once before being picked
 * from the ObserverManager::NextQueue, then only the last update is processed.
 */
class ObserverManager {
 public:
  static size_t getVersion() { return getInstance().version_; }

  static bool inManagerThread() { return inManagerThread_; }

  static void scheduleRefresh(Core::Ptr core, size_t minVersion) {
    if (core->getVersion() >= minVersion) {
      return;
    }

    auto updatesManager = getUpdatesManager();

    if (!updatesManager) {
      return;
    }

    auto& instance = getInstance();

    SharedMutexReadPriority::ReadHolder rh(instance.versionMutex_);

    updatesManager->scheduleCurrent(
        [core = std::move(core), &instance, rh = std::move(rh)]() {
          core->refresh(instance.version_);
        });
  }

  static void scheduleRefreshNewVersion(Function<Core::Ptr()> coreFunc) {
    auto updatesManager = getUpdatesManager();

    if (!updatesManager) {
      return;
    }

    updatesManager->scheduleNext(std::move(coreFunc));
  }

  static void initCore(Core::Ptr core) {
    DCHECK(core->getVersion() == 0);

    auto& instance = getInstance();

    folly::fibers::runInMainContext([&] {
      auto inManagerThread = std::exchange(inManagerThread_, true);
      SCOPE_EXIT { inManagerThread_ = inManagerThread; };

      SharedMutexReadPriority::ReadHolder rh(instance.versionMutex_);

      core->refresh(instance.version_);
    });
  }

  static void waitForAllUpdates();

  class DependencyRecorder {
   public:
    using DependencySet = std::unordered_set<Core::Ptr>;
    struct Dependencies {
      explicit Dependencies(const Core& core_) : core(core_) {}

      DependencySet dependencies;
      const Core& core;
    };

    explicit DependencyRecorder(const Core& core) : dependencies_(core) {
      DCHECK(inManagerThread());

      previousDepedencies_ = currentDependencies_;
      currentDependencies_ = &dependencies_;
    }

    static bool isActive() { return currentDependencies_; }

    template <typename F>
    static invoke_result_t<F> withDependencyRecordingDisabled(F f) {
      auto* const dependencies = std::exchange(currentDependencies_, nullptr);
      SCOPE_EXIT { currentDependencies_ = dependencies; };

      return f();
    }

    static void markDependency(Core::Ptr dependency) {
      DCHECK(inManagerThread());
      DCHECK(currentDependencies_);

      currentDependencies_->dependencies.insert(std::move(dependency));
    }

    static void markRefreshDependency(const Core& core) {
      if (!kIsDebug) {
        return;
      }
      if (!currentDependencies_) {
        return;
      }

      getInstance().cycleDetector_.withLock([&](CycleDetector& cycleDetector) {
        bool hasCycle =
            !cycleDetector.addEdge(&currentDependencies_->core, &core);
        if (hasCycle) {
          LOG(FATAL) << "Observer cycle detected.";
        }
      });
    }

    static void unmarkRefreshDependency(const Core& core) {
      if (!kIsDebug) {
        return;
      }
      if (!currentDependencies_) {
        return;
      }

      getInstance().cycleDetector_.withLock([&](CycleDetector& cycleDetector) {
        cycleDetector.removeEdge(&currentDependencies_->core, &core);
      });
    }

    DependencySet release() {
      DCHECK(currentDependencies_ == &dependencies_);
      std::swap(currentDependencies_, previousDepedencies_);
      previousDepedencies_ = nullptr;

      return std::move(dependencies_.dependencies);
    }

    ~DependencyRecorder() {
      if (currentDependencies_ == &dependencies_) {
        release();
      }
    }

   private:
    Dependencies dependencies_;
    Dependencies* previousDepedencies_;

    static thread_local Dependencies* currentDependencies_;
  };

 private:
  ObserverManager() {}

  class UpdatesManager {
   public:
    UpdatesManager();
    ~UpdatesManager();
    void scheduleCurrent(Function<void()>);
    void scheduleNext(Function<Core::Ptr()>);
    void waitForAllUpdates();

   private:
    class CurrentQueue;
    class NextQueue;

    std::unique_ptr<CurrentQueue> currentQueue_;
    std::unique_ptr<NextQueue> nextQueue_;
  };
  struct Singleton;

  static ObserverManager& getInstance();
  static std::shared_ptr<UpdatesManager> getUpdatesManager();
  static thread_local bool inManagerThread_;

  /**
   * Version mutex is used to make sure all updates are processed from the
   * CurrentQueue, before bumping the version and moving to the NextQueue.
   *
   * To achieve this every task added to CurrentQueue holds a reader lock.
   * NextQueue grabs a writer lock before bumping the version, so it can only
   * happen if CurrentQueue is empty (notice that we use read-priority shared
   * mutex).
   */
  SharedMutexReadPriority versionMutex_;
  std::atomic<size_t> version_{1};

  using CycleDetector = GraphCycleDetector<const Core*>;
  folly::Synchronized<CycleDetector, std::mutex> cycleDetector_;
};
} // namespace observer_detail
} // namespace folly
