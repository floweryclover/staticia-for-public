//
// Created by floweryclover on 2025-08-11.
//

#include "F_Executor.h"
#include "F_Threads.h"
#include <godot_cpp/variant/utility_functions.hpp>

using namespace Core;
using namespace godot;

F_Executor::F_Executor(const uint32_t workerThreadCount)
    : ThreadResults{ std::make_unique<ExecutorThreadResult[]>(workerThreadCount) },
      ThreadContexts{ std::make_unique<WorkerThreadContext[]>(workerThreadCount) },
      WorkerThreadCount{ workerThreadCount },
      multiThreadUpdateContext_{ nullptr },
      work_{
          [](const uint32_t)
          {
          }
      }
{
    for (int threadId = 1; threadId <= workerThreadCount; ++threadId)
    {
        ThreadContexts[threadId - 1].WorkingFlag.store(true, std::memory_order_release);
        ThreadContexts[threadId - 1].Thread = std::thread(&F_Executor::WorkerThreadBody, this, threadId);
        ThreadContexts[threadId - 1].WorkingFlag.wait(true, std::memory_order_acquire);
    }
    F_Threads::GetSingleton().LockRegistration();
}

F_Executor::~F_Executor()
{
    if (shouldStop_.load(std::memory_order_relaxed))
    {
        return;
    }

    shouldStop_.store(true, std::memory_order_relaxed);
    work_ = [](const uint32_t)
    {
    };

    for (int threadId = 1; threadId <= WorkerThreadCount; ++threadId)
    {
        ThreadContexts[threadId - 1].WorkingFlag.store(true, std::memory_order_release);
        ThreadContexts[threadId - 1].WorkingFlag.notify_one();
        ThreadContexts[threadId - 1].Thread.join();
    }

    F_Threads::GetSingleton().UnlockRegistration();
}

void F_Executor::WorkerThreadBody(const int threadId)
{
    UtilityFunctions::print("Worker Thread ", threadId, " start");
    F_Threads::GetSingleton().RegisterCurrentThread(threadId);

    while (!shouldStop_.load(std::memory_order_relaxed))
    {
        ThreadContexts[threadId - 1].WorkingFlag.wait(false, std::memory_order_acquire);
        if (shouldStop_.load(std::memory_order_relaxed))
        {
            break;
        }

        work_(threadId);
        ThreadContexts[threadId - 1].WorkingFlag.store(false, std::memory_order_release);
        ThreadContexts[threadId - 1].WorkingFlag.notify_one();
    }

    UtilityFunctions::print("Worker Thread ", threadId, " end");

    ThreadContexts[threadId - 1].WorkingFlag.store(false, std::memory_order_release);
    ThreadContexts[threadId - 1].WorkingFlag.notify_one();
    F_Threads::GetSingleton().UnregisterCurrentThread();
}
