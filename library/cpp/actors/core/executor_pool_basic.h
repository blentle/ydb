#pragma once

#include "actorsystem.h"
#include "executor_thread.h"
#include "scheduler_queue.h"
#include "executor_pool_base.h"
#include <library/cpp/actors/util/unordered_cache.h>
#include <library/cpp/actors/util/threadparkpad.h>
#include <library/cpp/monlib/dynamic_counters/counters.h>

#include <util/system/mutex.h>

namespace NActors {
    class TBasicExecutorPool: public TExecutorPoolBase {
        struct TThreadCtx {
            TAutoPtr<TExecutorThread> Thread;
            TThreadParkPad Pad;
            TThreadParkPad BlockedPad;
            TAtomic WaitingFlag;
            TAtomic BlockedFlag;

            // different threads must spin/block on different cache-lines.
            // we add some padding bytes to enforce this rule
            static const size_t SizeWithoutPadding = sizeof(TAutoPtr<TExecutorThread>) + 2 * sizeof(TThreadParkPad) + 2 * sizeof(TAtomic);
            ui8 Padding[64 - SizeWithoutPadding];
            static_assert(64 >= SizeWithoutPadding);

            enum EWaitState {
                WS_NONE,
                WS_ACTIVE,
                WS_BLOCKED,
                WS_RUNNING
            };

            enum EBlockedState {
                BS_NONE,
                BS_BLOCKING,
                BS_BLOCKED
            };

            TThreadCtx()
                : WaitingFlag(WS_NONE)
                , BlockedFlag(BS_NONE)
            {
            }
        };

        struct TTimers {
            NHPTimer::STime Elapsed = 0;
            NHPTimer::STime Parked = 0;
            NHPTimer::STime Blocked = 0;
            NHPTimer::STime HPStart = GetCycleCountFast();
            NHPTimer::STime HPNow;
        };

        const ui64 SpinThreshold;
        const ui64 SpinThresholdCycles;

        TArrayHolder<TThreadCtx> Threads;

        TArrayHolder<NSchedulerQueue::TReader> ScheduleReaders;
        TArrayHolder<NSchedulerQueue::TWriter> ScheduleWriters;

        const TString PoolName;
        const TDuration TimePerMailbox;
        const ui32 EventsPerMailbox;

        const int RealtimePriority;

        TAtomic ThreadUtilization;
        TAtomic MaxUtilizationCounter;
        TAtomic MaxUtilizationAccumulator;

        TAtomic ThreadCount;
        TMutex ChangeThreadsLock;

    public:
        struct TSemaphore {
            i64 OldSemaphore = 0; // 34 bits
            // Sign bit
            i8 Reserved1 = 0; // 5 bits
            i16 CurrentSleepThreadCount = 0; // 16 bits
            i8 Reserved2 = 0; // 8 bits

            inline i64 ConverToI64() {
                i64 value = (1ll << 34) + OldSemaphore;
                return value
                    | ((i64)Reserved1 << 35)
                    | ((i64)CurrentSleepThreadCount << 40)
                    | ((i64)Reserved2 << 56);
            }

            static inline TSemaphore GetSemaphore(i64 value) {
                TSemaphore semaphore;
                semaphore.OldSemaphore = (value & 0x7ffffffffll) - (1ll << 34);
                semaphore.Reserved1 = (value >> 35) & 0x1f;
                semaphore.CurrentSleepThreadCount = (value >> 40) & 0xffff;
                semaphore.Reserved2 = (value >> 56) & 0xff;
                return semaphore;
            }
        };

        static constexpr TDuration DEFAULT_TIME_PER_MAILBOX = TBasicExecutorPoolConfig::DEFAULT_TIME_PER_MAILBOX;
        static constexpr ui32 DEFAULT_EVENTS_PER_MAILBOX = TBasicExecutorPoolConfig::DEFAULT_EVENTS_PER_MAILBOX;

        TBasicExecutorPool(ui32 poolId,
                           ui32 threads,
                           ui64 spinThreshold,
                           const TString& poolName = "",
                           TAffinity* affinity = nullptr,
                           TDuration timePerMailbox = DEFAULT_TIME_PER_MAILBOX,
                           ui32 eventsPerMailbox = DEFAULT_EVENTS_PER_MAILBOX,
                           int realtimePriority = 0,
                           ui32 maxActivityType = 1);
        explicit TBasicExecutorPool(const TBasicExecutorPoolConfig& cfg);
        ~TBasicExecutorPool();

        ui32 GetReadyActivation(TWorkerContext& wctx, ui64 revolvingReadCounter) override;

        void Schedule(TInstant deadline, TAutoPtr<IEventHandle> ev, ISchedulerCookie* cookie, TWorkerId workerId) override;
        void Schedule(TMonotonic deadline, TAutoPtr<IEventHandle> ev, ISchedulerCookie* cookie, TWorkerId workerId) override;
        void Schedule(TDuration delta, TAutoPtr<IEventHandle> ev, ISchedulerCookie* cookie, TWorkerId workerId) override;

        void ScheduleActivationEx(ui32 activation, ui64 revolvingWriteCounter) override;

        void Prepare(TActorSystem* actorSystem, NSchedulerQueue::TReader** scheduleReaders, ui32* scheduleSz) override;
        void Start() override;
        void PrepareStop() override;
        void Shutdown() override;

        void GetCurrentStats(TExecutorPoolStats& poolStats, TVector<TExecutorThreadStats>& statsCopy) const override;
        TString GetName() const override {
            return PoolName;
        }

        void SetRealTimeMode() const override;

        ui32 GetThreadCount() const;
        void SetThreadCount(ui32 threads);

    private:
        void WakeUpLoop();
        bool GoToWaiting(TThreadCtx& threadCtx, TTimers &timers, bool needToBlock);
        void GoToSpin(TThreadCtx& threadCtx);
        bool GoToSleep(TThreadCtx& threadCtx, TTimers &timers);
        bool GoToBeBlocked(TThreadCtx& threadCtx, TTimers &timers);
    };
}
