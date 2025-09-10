//
// Created by floweryclover on 2024-12-17.
//

#ifndef CORE_F_THREADS_H
#define CORE_F_THREADS_H

#include "I_Singleton.h"
#include "U_ErrorMacros.h"
#include <algorithm>
#include <mutex>
#include <vector>

namespace Core
{
    /**
     * ThreadId는 메인 스레드는 0으로, 워커 스레드는 1부터 연속적인 값으로 할당됨.
     * 스레드별 컨테이너를 필요로 하는 클래스가 있다면, GetThreadCount()만큼의 배열을 할당한 후 ThreadId를 인덱스로 사용하면
     * 스레드 안전한 컨테이너를 생성 가능할 것임.
     */
    class F_Threads final : public I_Singleton<F_Threads>
    {
    public:
        friend F_Threads& I_Singleton<F_Threads>::GetSingleton();

        constexpr static uint32_t MainThreadId = 0;
        constexpr static uint32_t UnregisteredThreadId = 0xffffffff;

        void RegisterCurrentThread(const uint32_t threadId)
        {
            std::lock_guard lock{ mutex_ };

            SCRASH_COND(registrationLocked_);
            SCRASH_COND_MSG(CurrentThreadId != UnregisteredThreadId, CurrentThreadId);
            CurrentThreadId = threadId;

            for (int existingThreadId = 0; existingThreadId < registeredThreads_.size(); ++existingThreadId)
            {
                SCRASH_COND_MSG(existingThreadId == threadId && registeredThreads_[existingThreadId], threadId);
            }

            // ReSharper disable once CppDFALoopConditionNotUpdated
            while (registeredThreads_.size() <= threadId)
            {
                registeredThreads_.push_back(false);
            }
            registeredThreads_[threadId] = true;
            CurrentThreadId = threadId;
        }

        void UnregisterCurrentThread()
        {
            std::lock_guard lock{ mutex_ };
            SCRASH_COND(!registrationLocked_);
            SCRASH_COND(CurrentThreadId == UnregisteredThreadId);
            registeredThreads_[CurrentThreadId] = false;
            if (CurrentThreadId == registeredThreads_.size() - 1)
            {
                registeredThreads_.pop_back();
            }
        }

        [[nodiscard]]
        uint32_t GetCurrentThreadId() const
        {
            SCRASH_COND(!registrationLocked_);
            SCRASH_COND(CurrentThreadId == UnregisteredThreadId);
            return CurrentThreadId;
        }

        [[nodiscard]]
        size_t GetThreadCount() const
        {
            SCRASH_COND(!registrationLocked_);
            const bool isAllThreadRegisteredContiguously = std::ranges::all_of(registeredThreads_,
                                                                               [](const bool isRegistered)
                                                                               {
                                                                                   return isRegistered;
                                                                               });
            SCRASH_COND(!isAllThreadRegisteredContiguously);

            return registeredThreads_.size();
        }

        void LockRegistration()
        {
            std::lock_guard lock{ mutex_ };
            SCRASH_COND(registrationLocked_);
            registrationLocked_ = true;
        }

        void UnlockRegistration()
        {
            std::lock_guard lock{ mutex_ };
            SCRASH_COND(!registrationLocked_);
            registrationLocked_ = false;
        }

    private:
        F_Threads() = default;

        inline static thread_local uint32_t CurrentThreadId = UnregisteredThreadId;
        std::vector<bool> registeredThreads_;
        bool registrationLocked_{ false };
        std::mutex mutex_;
    };

}

#endif // CORE_F_THREADS_H
