//
// Created by floweryclover on 2024-11-29.
//

#ifndef SETTLEMENTSIMULATORNATIVE_F_SYSTEMMANAGER_H
#define SETTLEMENTSIMULATORNATIVE_F_SYSTEMMANAGER_H

#include "S_System.h"
#include "F_MemoryPoolManager.h"
#include "F_ComponentContainer.h"
#include "F_Pathfinder.h"
#include <unordered_map>
#include <thread>
#include <atomic>
#include <typeindex>

class F_EntityManager;

class F_EventManager;

class F_SystemManager
{
private:
    struct CalculateThreadContext
    {
        CalculateThreadContext() : ShouldStop{ false }, Working { false }

        {}
        std::atomic_bool ShouldStop;
        std::atomic_bool Working;
    };

public:
    explicit F_SystemManager(
        F_EntityManager& entityManagerRef,
        F_EventManager& eventManagerRef,
        int threadCount);

    ~F_SystemManager() = default;

    F_SystemManager(const F_SystemManager&) = delete;

    F_SystemManager& operator=(const F_SystemManager&) = delete;

    F_SystemManager(F_SystemManager&&) = delete;

    F_SystemManager& operator=(F_SystemManager&&) = delete;

    template<IsSingleThreadSystem TSingleThreadSystem>
    void RegisterSingleThreadSystem()
    {
        for (const auto& systemBlueprintKv : singleThreadedSystemBlueprints_)
        {
            ERR_FAIL_COND(systemBlueprintKv.first == typeid(TSingleThreadSystem));
        }
        for (const auto& systemBlueprintKv : multiThreadedSystemBlueprints_)
        {
            ERR_FAIL_COND(systemBlueprintKv.first == typeid(TSingleThreadSystem));
        }

        singleThreadedSystemBlueprints_.emplace_back(typeid(TSingleThreadSystem), TSingleThreadSystem::SingleThreadSystemBlueprint);
    }

    template<IsMultiThreadSystem TMultiThreadSystem>
    void RegisterMultiThreadSystem()
    {
        for (const auto& systemBlueprintKv : singleThreadedSystemBlueprints_)
        {
            ERR_FAIL_COND(systemBlueprintKv.first == typeid(TMultiThreadSystem));
        }
        for (const auto& systemBlueprintKv : multiThreadedSystemBlueprints_)
        {
            ERR_FAIL_COND(systemBlueprintKv.first == typeid(TMultiThreadSystem));
        }

        multiThreadedSystemBlueprints_.emplace_back(typeid(TMultiThreadSystem), TMultiThreadSystem::MultiThreadSystemBlueprint);
        revisionDataFirstNodes_.emplace(typeid(TMultiThreadSystem), nullptr);
    }

    void Update(double deltaTime, uint64_t deltaTicks, uint64_t currentTick);

    void CalculateThreadBody(int threadId);

private:
    struct alignas(64) MultithreadWork
    {
        int SystemIndex;
        F_RawComponentContainer::RawConstIterator AxisComponentConstIterator;
    };

    F_EntityManager& EntityManagerRef;
    F_EventManager& EventManagerRef;
    std::vector<std::pair<std::type_index, const F_SingleThreadSystemBlueprint&>> singleThreadedSystemBlueprints_;
    std::vector<std::pair<std::type_index, const F_MultiThreadSystemBlueprint&>> multiThreadedSystemBlueprints_;
    std::unordered_map<std::type_index, std::atomic<F_RevisionDataNode*>> revisionDataFirstNodes_;

    std::vector<CalculateThreadContext> calculateThreadContexts_;
    std::atomic_int workingThreadCount_;
    std::atomic_bool workDone_;
    const int ThreadCount;

    std::atomic<MultithreadWork> currentMultiThreadWork_;
    double currentDeltaTime_;
    uint64_t currentDeltaTicks_;
    uint64_t currentTick_;
    F_Pathfinder mainThreadPathfinder_;
};

#endif //SETTLEMENTSIMULATORNATIVE_F_SYSTEMMANAGER_H
