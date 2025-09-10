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
      currentMultithreadWorkCursor_{ 0, nullptr, 0 },
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
        const auto rawComponentContainerPtr = EntityManagerRef.GetRawComponentContainer(multiThreadedSystemBlueprints_[0]
                                                                                            .second
                                                                                            .AxisComponentTypeIndex);
        currentMultithreadWorkCursor_.SystemIndex = 0;
        currentMultithreadWorkCursor_.EntityComponentMemoryPoolPtr = (rawComponentContainerPtr
                                                                      != nullptr ? &rawComponentContainerPtr->GetMemoryPool() : nullptr);
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

        { // shared lock scope
            std::shared_lock<std::shared_mutex> sharedLock(currentMultithreadWorkCursorMutex_);
            const int currentSystemIndex = currentMultithreadWorkCursor_.SystemIndex;
            if (currentMultithreadWorkCursor_.SystemIndex == multiThreadedSystemBlueprints_.size())
            {
                calculateThreadContext.Working.store(false, std::memory_order_release);
                if (workingThreadCount_.fetch_add(-1, std::memory_order_acq_rel) == 1)
                {
                    workDone_.store(true, std::memory_order_release);
                    workDone_.notify_one();
                }
                continue;
            }

            const auto memoryPoolBlockCount = (currentMultithreadWorkCursor_.EntityComponentMemoryPoolPtr != nullptr ?
                                               currentMultithreadWorkCursor_.EntityComponentMemoryPoolPtr
                                                   ->GetPageCount()
                                               * currentMultithreadWorkCursor_.EntityComponentMemoryPoolPtr
                                                   ->BlocksPerPage : 0);
            const auto memoryBlockIndex = currentMultithreadWorkCursor_.EntityComponentMemoryBlockIndex
                .fetch_add(1, std::memory_order_relaxed);

            if (currentMultithreadWorkCursor_.EntityComponentMemoryPoolPtr == nullptr
                || memoryBlockIndex >= memoryPoolBlockCount)
            {
                sharedLock.unlock();
                std::unique_lock<std::shared_mutex> uniqueLock(currentMultithreadWorkCursorMutex_);
                if (currentMultithreadWorkCursor_.SystemIndex == currentSystemIndex)
                {
                    currentMultithreadWorkCursor_.SystemIndex += 1;
                    const auto rawComponentContainerPtr = (currentSystemIndex + 1
                                                           < multiThreadedSystemBlueprints_.size() ? EntityManagerRef.GetRawComponentContainer(
                        multiThreadedSystemBlueprints_[currentSystemIndex + 1].second
                            .AxisComponentTypeIndex) : nullptr);
                    currentMultithreadWorkCursor_.EntityComponentMemoryPoolPtr = (rawComponentContainerPtr
                                                                                  != nullptr ? &rawComponentContainerPtr
                        ->GetMemoryPool() : nullptr);
                    currentMultithreadWorkCursor_.EntityComponentMemoryBlockIndex.store(0, std::memory_order_relaxed);
                }
                continue;
            }

            const void* const memoryBlockPtr = currentMultithreadWorkCursor_.EntityComponentMemoryPoolPtr
                ->GetMemoryBlockByIndex(memoryBlockIndex);
            if (memoryBlockPtr == nullptr)
            {

                continue;
            }
            const F_Entity axisComponentOwnerEntity = *reinterpret_cast<const F_Entity*>(memoryBlockPtr);
            const void* const axisComponentPtr = (static_cast<const char*>(memoryBlockPtr) + sizeof(F_Entity));
            const std::type_index currentSystemTypeIndex = multiThreadedSystemBlueprints_[currentSystemIndex].first;
            const ProcessFunction processFunction = multiThreadedSystemBlueprints_[currentSystemIndex].second
                .ProcessFunction;
            processFunction(F_ProcessParams{ axisComponentPtr,
                                             axisComponentOwnerEntity,
                                             F_RevisionDataWriteContext{ revisionDataFirstNodes_.at(
                                                 currentSystemTypeIndex) },
                                             pathfinder,
                                             EntityManagerRef,
                                             currentDeltaTime_,
                                             currentDeltaTicks_,
                                             currentTick_ });
        }
    }
    std::cout << "Thread " << threadId << " exit" << std::endl;
}
