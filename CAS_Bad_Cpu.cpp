//
// Created by floweryclover on 2024-11-29.
//

#include "F_SystemManager.h"
#include "U_ThreadInfo.h"
#include "F_EntityManager.h"
#include "F_EventManager.h"
#include "U_Hash.h"
#include "F_Map.h"
#include "F_Pathfinder.h"
#include <thread>
#include <iostream>

F_SystemManager::F_SystemManager(F_EntityManager& entityManagerRef,
                                 F_EventManager& eventManagerRef,
                                 const int threadCount)
    : EntityManagerRef{ entityManagerRef },
      EventManagerRef{ eventManagerRef },
      ThreadCount{ threadCount },
      calculateThreadContexts_{ static_cast<size_t>(threadCount + 1) },
      workingThreadCount_{ 0 },
      mainThreadPathfinder_{ *entityManagerRef.GetGlobalObject<F_Map>("F_Map"_h), 0 },
      currentMultiThreadWork_{{ 0,
                                F_RawComponentContainer::RawConstIterator{ nullptr,
                                                                           F_MemoryPool::const_iterator{ nullptr,
                                                                                                         0,
                                                                                                         0 }}}},
      currentDeltaTime_{ 0.0 },
      currentDeltaTicks_{ 0 },
      currentTick_{ 0 }
{
    for (int threadId = 1; threadId <= threadCount; threadId++)
    { ;
        std::thread calculateThread(&F_SystemManager::CalculateThreadBody, this, threadId);
        calculateThread.detach();
    }
}

void F_SystemManager::Update(const double deltaTime, const uint64_t deltaTicks, const uint64_t currentTick)
{
    if (multiThreadedSystemBlueprints_.empty() && singleThreadedSystemBlueprints_.empty())
    {
        return;
    }
    currentDeltaTime_ = deltaTime;
    currentDeltaTicks_ = deltaTicks;
    currentTick_ = currentTick;
    if (!multiThreadedSystemBlueprints_.empty())
    {
        currentMultiThreadWork_.store({ 0,
                                        EntityManagerRef.GetRawComponentBeginConstIterator(
                                            multiThreadedSystemBlueprints_.at(0).second.AxisComponentTypeIndex) },
                                      std::memory_order_relaxed);
        workDone_.store(false, std::memory_order_relaxed);
        workingThreadCount_.store(ThreadCount, std::memory_order_relaxed);
        for (auto& calculateThreadContext : calculateThreadContexts_)
        {
            calculateThreadContext.Working.store(true, std::memory_order_release);
            calculateThreadContext.Working.notify_one();
        }
    }

    if (!multiThreadedSystemBlueprints_.empty())
    {
        while (!workDone_.load(std::memory_order_relaxed))
        {
            workDone_.wait(false, std::memory_order_acquire);
        }
    }

    for (auto kv : singleThreadedSystemBlueprints_)
    {
        kv.second
            .ProcessAndApplyFunction(F_ProcessAndApplyParams{ EntityManagerRef,
                                                              EventManagerRef,
                                                              deltaTime,
                                                              deltaTicks,
                                                              currentTick });
    }

    for (auto kv : multiThreadedSystemBlueprints_)
    {
        kv.second
            .ApplyFunction(F_ApplyParams{ F_RevisionDataReadContext{ revisionDataFirstNodes_.at(kv.first) },
                                          EntityManagerRef,
                                          EventManagerRef,
                                          mainThreadPathfinder_,
                                          deltaTime,
                                          deltaTicks,
                                          currentTick });
        kv.second.ReleaseRevisionDataNodeFunction(revisionDataFirstNodes_.at(kv.first));
        revisionDataFirstNodes_.at(kv.first) = nullptr;
    }

}

void F_SystemManager::CalculateThreadBody(const int threadId)
{
    CalculateThreadContext& calculateThreadContext = calculateThreadContexts_.at(threadId);
    std::cout << "Thread " << threadId << " start" << std::endl;
    U_ThreadInfo::RegisterCurrentThread(threadId);
    const F_Map* mapPtr = static_cast<const F_EntityManager&>(EntityManagerRef).GetGlobalObject<F_Map>("F_Map"_h);
    F_Pathfinder pathfinder{ *mapPtr, threadId };

    while (!calculateThreadContext.ShouldStop.load(std::memory_order_relaxed))
    {
        calculateThreadContext.Working.wait(false, std::memory_order_acquire);
        if (calculateThreadContext.ShouldStop.load(std::memory_order_relaxed))
        {
            break;
        }

        MultithreadWork currentMultithreadWork = currentMultiThreadWork_.load(std::memory_order_relaxed);
        while (true)
        {
            if (currentMultithreadWork.SystemIndex == multiThreadedSystemBlueprints_.size())
            {
                break;
            }

            MultithreadWork nextMultithreadWork{ 0,
                                                 F_RawComponentContainer::RawConstIterator{ nullptr,
                                                                                            F_MemoryPool::const_iterator{
                                                                                                nullptr,
                                                                                                0,
                                                                                                0 }}};

            if (currentMultithreadWork.AxisComponentConstIterator.IsEnd())
            {
                nextMultithreadWork.SystemIndex = currentMultithreadWork.SystemIndex + 1;
                if (currentMultithreadWork.SystemIndex < multiThreadedSystemBlueprints_.size() - 1)
                {
                    nextMultithreadWork.AxisComponentConstIterator = EntityManagerRef.GetRawComponentBeginConstIterator(
                        multiThreadedSystemBlueprints_.at(currentMultithreadWork.SystemIndex + 1)
                            .second
                            .AxisComponentTypeIndex);
                }
            }
            else
            {
                nextMultithreadWork.SystemIndex = currentMultithreadWork.SystemIndex;
                nextMultithreadWork.AxisComponentConstIterator = currentMultithreadWork.AxisComponentConstIterator;
                ++nextMultithreadWork.AxisComponentConstIterator;
            }

            if (currentMultiThreadWork_.compare_exchange_weak(currentMultithreadWork,
                                                              nextMultithreadWork,
                                                              std::memory_order_relaxed,
                                                              std::memory_order_relaxed))
            {
                if (currentMultithreadWork.AxisComponentConstIterator.IsEnd())
                {
                    continue;
                }
                else
                {
                    break;
                }

            }
        }

        if (currentMultithreadWork.SystemIndex == static_cast<int>(multiThreadedSystemBlueprints_.size())
            || currentMultithreadWork.AxisComponentConstIterator.IsEnd())
        {
            calculateThreadContext.Working.store(false, std::memory_order_release);
            if (workingThreadCount_.fetch_add(-1, std::memory_order_acq_rel) == 1)
            {
                workDone_.store(true, std::memory_order_release);
                workDone_.notify_one();
            }
            continue;
        }

        const int currentSystemIndex = currentMultithreadWork.SystemIndex;
        const std::type_index currentSystemTypeIndex = multiThreadedSystemBlueprints_[currentSystemIndex].first;
        const ProcessFunction processFunction = multiThreadedSystemBlueprints_[currentSystemIndex].second
            .ProcessFunction;
        const void* const axisComponentPtr = (*currentMultithreadWork.AxisComponentConstIterator).second;
        const F_Entity entity = (*currentMultithreadWork.AxisComponentConstIterator).first;
        processFunction(F_ProcessParams{ axisComponentPtr,
                                         entity,
                                         F_RevisionDataWriteContext{ revisionDataFirstNodes_.at(currentSystemTypeIndex) },
                                         pathfinder,
                                         EntityManagerRef,
                                         currentDeltaTime_,
                                         currentDeltaTicks_,
                                         currentTick_ });
    }
    std::cout << "Thread " << threadId << " exit" << std::endl;
}
