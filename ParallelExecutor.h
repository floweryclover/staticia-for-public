//
// Created by floweryclover on 2025-07-10.
//

#ifndef CORE_F_EXECUTOR_H
#define CORE_F_EXECUTOR_H

#include "F_EntityManager.h"
#include "U_Concurrency.h"
#include "F_System.h"
#include <functional>
#include <span>
#include <optional>

namespace Core
{
    enum class E_Execution : uint8_t
    {
        TotallyImmutable,
        MutableAxis,
    };

    template<typename TAxis, E_Execution Execution>
    using TExecutionAxis = std::conditional_t<Execution == E_Execution::TotallyImmutable, const TAxis&, TAxis&>;

    template<typename TEvent, typename TExecutionResult, typename Task, E_Execution Execution>
    concept IsParallelForEventsTask = requires(Task task,
                                               TExecutionAxis<TEvent, Execution> event,
                                               const F_ImmutableContext& immutableContext)
    {
        { std::invoke(task, event, immutableContext) } -> std::same_as<std::optional<TExecutionResult>>;
    };

    template<typename TComponent, typename TExecutionResult, typename Task, E_Execution Execution>
    concept IsParallelForComponentsTask = requires(Task task,
                                                   F_Entity entity,
                                                   TExecutionAxis<TComponent, Execution> component,
                                                   const F_ImmutableContext& immutableContext)
    {
        { std::invoke(task, entity, component, immutableContext) } -> std::same_as<std::optional<TExecutionResult>>;
    };

    template<typename TExecutionResult, typename Task>
    concept IsParallelForWorkerThreadsTask = requires(Task task, const F_ImmutableContext& immutableContext)
    {
        { std::invoke(task, immutableContext) } -> std::same_as<std::optional<TExecutionResult>>;
    };

    class alignas(U_Concurrency::CacheLineSize) F_Executor final
    {
        struct ExecutorThreadResult;
        struct WorkerParameters;

    public:
        /**
         * @remarks 반드시 range based for에서만 사용하고, 임의로 반복자를 조작하거나 이용하지 말 것 - 직관적으로 의도대로 동작하지 않음.
         * @tparam TResult
         */
        template<typename TResult>
        class ExecutionResults;

        explicit F_Executor(uint32_t workerThreadCount);

        ~F_Executor();

        F_Executor(const F_Executor&) = delete;

        F_Executor(F_Executor&&) = delete;

        F_Executor& operator=(const F_Executor&) = delete;

        F_Executor& operator=(F_Executor&&) = delete;

        template<IsComponent TComponent,
            IsTriviallyCopyable TExecutionResult,
            E_Execution Execution = E_Execution::TotallyImmutable>
        ExecutionResults<TExecutionResult> ParallelForComponents(const F_MutableContext& context,
                                                                 auto&& task,
                                                                 size_t chunkSize)
            requires IsParallelForComponentsTask<TComponent, TExecutionResult, decltype(task), Execution>;

        template<IsEvent TEvent,
            IsTriviallyCopyable TExecutionResult,
            E_Execution Execution = E_Execution::TotallyImmutable>
        ExecutionResults<TExecutionResult> ParallelForEvents(const F_MutableContext& context,
                                                             auto&& task,
                                                             size_t chunkSize)
            requires IsParallelForEventsTask<TEvent, TExecutionResult, decltype(task), Execution>;

        template<IsTriviallyCopyable TExecutionResult>
        ExecutionResults<TExecutionResult> ParallelForWorkerThreads(const F_MutableContext& context, auto&& task)
            requires IsParallelForWorkerThreadsTask<TExecutionResult, decltype(task)>;

    private:
        struct WorkerThreadContext
        {
            std::thread Thread;
            std::atomic_bool WorkingFlag;
        };

        static constexpr size_t BaseThreadMemorySize = 1024;

        const std::unique_ptr<ExecutorThreadResult[]> ThreadResults;
        const std::unique_ptr<WorkerThreadContext[]> ThreadContexts;
        const uint32_t WorkerThreadCount;

        const F_MutableContext* multiThreadUpdateContext_;
        std::function<void(uint32_t)> work_;
        alignas(U_Concurrency::CacheLineSize) std::atomic_bool shouldStop_; // 메인 스레드에서 설정하는, 워커 스레드들의 완전한 종료 명령 상태.

        // Component인 경우 dense Index, Event인 경우 EventQueue에서의 Index.
        alignas(U_Concurrency::CacheLineSize) std::atomic_uint32_t multiThreadWorkIndex_;

        void WorkerThreadBody(int threadId);

        static void ExtendPageAtLeast(ExecutorThreadResult& threadResult, size_t atLeast);

        template<IsTriviallyCopyable TExecutionResult>
        ExecutionResults<TExecutionResult> ExecutorCommon(const F_MutableContext& context,
                                                          size_t chunkSize,
                                                          auto&& getAxis,
                                                          auto&& task);
    };

    struct F_Executor::ExecutorThreadResult final
    {
        size_t MemoryBlockSize = 0;
        size_t ResultElementCount = 0;
        std::unique_ptr<char[]> MemoryBlock;
    };

    template<typename TResult>
    class F_Executor::ExecutionResults final
    {
    public:
        class Iterator final
        {
        public:
            using value_type = TResult;
            using reference = const value_type&;
            using iterator_category = std::input_iterator_tag;

            explicit Iterator(const std::span<ExecutorThreadResult> threadContexts,
                              const uint32_t currentThreadResultIndex,
                              const uint32_t currentResultElementIndex)
                : threadResults_{ threadContexts },
                  currentThreadResultIndex_{ currentThreadResultIndex },
                  currentResultElementIndex_{ currentResultElementIndex }
            {
            }

            reference operator*() const
            {
                return *reinterpret_cast<value_type*>(
                    threadResults_[currentThreadResultIndex_].MemoryBlock.get() + currentResultElementIndex_ * sizeof(
                        TResult));
            }

            Iterator& operator++()
            {
                currentResultElementIndex_ += 1;
                while (currentThreadResultIndex_ < threadResults_.size() &&
                       currentResultElementIndex_ >= threadResults_[currentThreadResultIndex_].
                       ResultElementCount)
                {
                    currentResultElementIndex_ = 0;
                    currentThreadResultIndex_ += 1;
                }

                return *this;
            }

            /**
             * end()와의 비교 상황만 가정하여, == end()인 상황, 즉 out of range 상황을 감지함.
             * @return
             */
            bool operator==(const Iterator&) const
            {
                return currentThreadResultIndex_ >= threadResults_.size();
            }

            /**
             * end()와의 비교 상황만 가정하여, != end()인 상황, 즉 in range 상황을 감지함.
             * @return
             */
            bool operator!=(const Iterator&) const
            {
                return currentThreadResultIndex_ < threadResults_.size();
            }

        private:
            std::span<ExecutorThreadResult> threadResults_;
            uint32_t currentThreadResultIndex_;
            uint32_t currentResultElementIndex_;
        };

        explicit ExecutionResults(std::span<ExecutorThreadResult> threadContexts)
            : threadContexts_{ threadContexts }
        {
        }

        ~ExecutionResults() = default;

        ExecutionResults(const ExecutionResults&) = delete;

        ExecutionResults(ExecutionResults&&) = delete;

        ExecutionResults& operator=(const ExecutionResults&) = delete;

        ExecutionResults& operator=(ExecutionResults&&) = delete;

        Iterator begin() const
        {
            uint32_t nonEmptyThreadIndex = 0;
            for (nonEmptyThreadIndex = 0; nonEmptyThreadIndex < threadContexts_.size(); ++nonEmptyThreadIndex)
            {
                if (threadContexts_[nonEmptyThreadIndex].ResultElementCount > 0)
                {
                    break;
                }
            }

            return Iterator{ threadContexts_, nonEmptyThreadIndex, 0 };
        }

        Iterator end() const
        {
            return Iterator{ threadContexts_, static_cast<uint32_t>(threadContexts_.size()), 0 };
        }

    private:
        std::span<ExecutorThreadResult> threadContexts_;
    };

    struct F_Executor::WorkerParameters
    {
        const F_MutableContext& Mutable;
        const F_ImmutableContext& Immutable;
        const size_t ChunkSize;
    };

    template<IsComponent TComponent,
        IsTriviallyCopyable TExecutionResult,
        E_Execution Execution>
    F_Executor::ExecutionResults<TExecutionResult> F_Executor::ParallelForComponents(
        const F_MutableContext& context,
        auto&& task,
        const size_t chunkSize = 32)
        requires IsParallelForComponentsTask<TComponent, TExecutionResult, decltype(task), Execution>
    {
        return ExecutorCommon<TExecutionResult>(
            context,
            chunkSize,
            [](const WorkerParameters& workerParameters, const uint32_t index)
            {
                if constexpr (Execution == E_Execution::TotallyImmutable)
                {
                    return workerParameters.Immutable.EntityManager.GetComponentFromDenseIndex<TComponent>(index);
                }
                else
                {
                    return workerParameters.Mutable.EntityManager.GetComponentFromDenseIndex<TComponent>(index);
                }
            },
            [&task](const F_Entity entity,
                    TExecutionAxis<TComponent, Execution> component,
                    const WorkerParameters& workerParameters)
            {
                return task(entity, component, workerParameters.Immutable);
            });
    }

    template<IsEvent TEvent,
        IsTriviallyCopyable TExecutionResult,
        E_Execution Execution>
    F_Executor::ExecutionResults<TExecutionResult> F_Executor::ParallelForEvents(
        const F_MutableContext& context,
        auto&& task,
        const size_t chunkSize = 32)
        requires IsParallelForEventsTask<TEvent, TExecutionResult, decltype(task), Execution>
    {
        return ExecutorCommon<TExecutionResult>(
            context,
            chunkSize,
            [](const WorkerParameters& workerParameters,
               const uint32_t index) -> std::pair<int, std::conditional_t<Execution == E_Execution::TotallyImmutable,
            const TEvent*, TEvent*>>
            {
                if constexpr (Execution == E_Execution::TotallyImmutable)
                {
                    return { 0, workerParameters.Immutable.EventManager.GetEventFromIndex<TEvent>(index) };
                }
                else
                {
                    return { 0, workerParameters.Mutable.EventManager.GetEventFromIndex<TEvent>(index) };
                }
            },
            [&task](const int, TExecutionAxis<TEvent, Execution> event, const WorkerParameters& workerParameters)
            {
                return task(event, workerParameters.Immutable);
            });
    }

    template<IsTriviallyCopyable TExecutionResult> F_Executor::ExecutionResults<TExecutionResult> F_Executor::
    ParallelForWorkerThreads(const F_MutableContext& context, auto&& task)
    requires IsParallelForWorkerThreadsTask<TExecutionResult, decltype(task)>
    {
        work_ = [this, &context, &task](const uint32_t threadId)
        {
            auto& threadResult = ThreadResults[threadId - 1];
            const auto result = task(static_cast<F_ImmutableContext>(context));
            if (!result)
            {
                return;
            }

            ExtendPageAtLeast(threadResult, sizeof(TExecutionResult));
            memcpy(
                threadResult.MemoryBlock.get() + sizeof(TExecutionResult),
                &*result,
                sizeof(TExecutionResult));
            threadResult.ResultElementCount = 1;
        };

        multiThreadUpdateContext_ = &context;
        multiThreadWorkIndex_.store(0, std::memory_order_relaxed);

        for (int threadId = 1; threadId <= WorkerThreadCount; ++threadId)
        {
            ThreadResults[threadId - 1].ResultElementCount = 0;
            ThreadContexts[threadId - 1].WorkingFlag.store(true, std::memory_order_release);
            ThreadContexts[threadId - 1].WorkingFlag.notify_one();
        }
        for (int threadId = 1; threadId <= WorkerThreadCount; ++threadId)
        {
            ThreadContexts[threadId - 1].WorkingFlag.wait(true, std::memory_order_acquire);
        }

        return ExecutionResults<TExecutionResult>{ std::span{ ThreadResults.get(), WorkerThreadCount } };
    }

    inline void F_Executor::ExtendPageAtLeast(ExecutorThreadResult& threadResult, const size_t atLeast)
    {
        while (threadResult.MemoryBlockSize < atLeast)
        {
            const size_t oldSize = threadResult.MemoryBlockSize;
            const size_t newSize = threadResult.MemoryBlockSize == 0
                                       ? BaseThreadMemorySize
                                       : threadResult.MemoryBlockSize * 2;
            auto newBlock = std::make_unique<char[]>(newSize);
            memcpy(newBlock.get(), threadResult.MemoryBlock.get(), oldSize);
            threadResult.MemoryBlock = std::move(newBlock);
            threadResult.MemoryBlockSize = newSize;
        }
    }

    template<IsTriviallyCopyable TExecutionResult>
    F_Executor::ExecutionResults<TExecutionResult> F_Executor::ExecutorCommon(
        const F_MutableContext& context,
        const size_t chunkSize,
        auto&& getAxis,
        auto&& task)
    {
        const auto workerParameters = WorkerParameters{ context, static_cast<F_ImmutableContext>(context), chunkSize };

        work_ = [this, &workerParameters, &task, &getAxis](const uint32_t threadId)
        {
            auto& threadResult = ThreadResults[threadId - 1];
            while (true)
            {
                const auto workEnd = multiThreadWorkIndex_.fetch_add(workerParameters.ChunkSize,
                                                                     std::memory_order_relaxed);
                const auto workBegin = workEnd - workerParameters.ChunkSize;

                for (uint32_t i = static_cast<uint32_t>(workBegin); i < workEnd; ++i)
                {
                    const auto axis = getAxis(workerParameters, i);
                    if (!axis.second)
                    {
                        return;
                    }

                    const auto result = task(axis.first, *axis.second, workerParameters);
                    if (!result)
                    {
                        continue;
                    }

                    ExtendPageAtLeast(threadResult, sizeof(TExecutionResult) * (threadResult.ResultElementCount + 1));
                    memcpy(
                        threadResult.MemoryBlock.get() + sizeof(TExecutionResult) * threadResult.ResultElementCount,
                        &*result,
                        sizeof(TExecutionResult));
                    threadResult.ResultElementCount += 1;
                }
            }
        };

        multiThreadUpdateContext_ = &context;
        multiThreadWorkIndex_.store(0, std::memory_order_relaxed);

        for (int threadId = 1; threadId <= WorkerThreadCount; ++threadId)
        {
            ThreadResults[threadId - 1].ResultElementCount = 0;
            ThreadContexts[threadId - 1].WorkingFlag.store(true, std::memory_order_release);
            ThreadContexts[threadId - 1].WorkingFlag.notify_one();
        }
        for (int threadId = 1; threadId <= WorkerThreadCount; ++threadId)
        {
            ThreadContexts[threadId - 1].WorkingFlag.wait(true, std::memory_order_acquire);
        }

        return ExecutionResults<TExecutionResult>{ std::span{ ThreadResults.get(), WorkerThreadCount } };
    }
}

#endif // CORE_F_EXECUTOR_H
