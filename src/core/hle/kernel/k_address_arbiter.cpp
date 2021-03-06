// Copyright 2021 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/arm/exclusive_monitor.h"
#include "core/core.h"
#include "core/hle/kernel/k_address_arbiter.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/k_scoped_scheduler_lock_and_sleep.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/svc_results.h"
#include "core/hle/kernel/time_manager.h"
#include "core/memory.h"

namespace Kernel {

KAddressArbiter::KAddressArbiter(Core::System& system_)
    : system{system_}, kernel{system.Kernel()} {}
KAddressArbiter::~KAddressArbiter() = default;

namespace {

bool ReadFromUser(Core::System& system, s32* out, VAddr address) {
    *out = system.Memory().Read32(address);
    return true;
}

bool DecrementIfLessThan(Core::System& system, s32* out, VAddr address, s32 value) {
    auto& monitor = system.Monitor();
    const auto current_core = system.CurrentCoreIndex();

    // TODO(bunnei): We should disable interrupts here via KScopedInterruptDisable.
    // TODO(bunnei): We should call CanAccessAtomic(..) here.

    // Load the value from the address.
    const s32 current_value = static_cast<s32>(monitor.ExclusiveRead32(current_core, address));

    // Compare it to the desired one.
    if (current_value < value) {
        // If less than, we want to try to decrement.
        const s32 decrement_value = current_value - 1;

        // Decrement and try to store.
        if (!monitor.ExclusiveWrite32(current_core, address, static_cast<u32>(decrement_value))) {
            // If we failed to store, try again.
            DecrementIfLessThan(system, out, address, value);
        }
    } else {
        // Otherwise, clear our exclusive hold and finish
        monitor.ClearExclusive();
    }

    // We're done.
    *out = current_value;
    return true;
}

bool UpdateIfEqual(Core::System& system, s32* out, VAddr address, s32 value, s32 new_value) {
    auto& monitor = system.Monitor();
    const auto current_core = system.CurrentCoreIndex();

    // TODO(bunnei): We should disable interrupts here via KScopedInterruptDisable.
    // TODO(bunnei): We should call CanAccessAtomic(..) here.

    // Load the value from the address.
    const s32 current_value = static_cast<s32>(monitor.ExclusiveRead32(current_core, address));

    // Compare it to the desired one.
    if (current_value == value) {
        // If equal, we want to try to write the new value.

        // Try to store.
        if (!monitor.ExclusiveWrite32(current_core, address, static_cast<u32>(new_value))) {
            // If we failed to store, try again.
            UpdateIfEqual(system, out, address, value, new_value);
        }
    } else {
        // Otherwise, clear our exclusive hold and finish.
        monitor.ClearExclusive();
    }

    // We're done.
    *out = current_value;
    return true;
}

} // namespace

ResultCode KAddressArbiter::Signal(VAddr addr, s32 count) {
    // Perform signaling.
    s32 num_waiters{};
    {
        KScopedSchedulerLock sl(kernel);

        auto it = thread_tree.nfind_light({addr, -1});
        while ((it != thread_tree.end()) && (count <= 0 || num_waiters < count) &&
               (it->GetAddressArbiterKey() == addr)) {
            KThread* target_thread = std::addressof(*it);
            target_thread->SetSyncedObject(nullptr, RESULT_SUCCESS);

            ASSERT(target_thread->IsWaitingForAddressArbiter());
            target_thread->Wakeup();

            it = thread_tree.erase(it);
            target_thread->ClearAddressArbiter();
            ++num_waiters;
        }
    }
    return RESULT_SUCCESS;
}

ResultCode KAddressArbiter::SignalAndIncrementIfEqual(VAddr addr, s32 value, s32 count) {
    // Perform signaling.
    s32 num_waiters{};
    {
        KScopedSchedulerLock sl(kernel);

        // Check the userspace value.
        s32 user_value{};
        R_UNLESS(UpdateIfEqual(system, std::addressof(user_value), addr, value, value + 1),
                 Svc::ResultInvalidCurrentMemory);

        if (user_value != value) {
            return Svc::ResultInvalidState;
        }

        auto it = thread_tree.nfind_light({addr, -1});
        while ((it != thread_tree.end()) && (count <= 0 || num_waiters < count) &&
               (it->GetAddressArbiterKey() == addr)) {
            KThread* target_thread = std::addressof(*it);
            target_thread->SetSyncedObject(nullptr, RESULT_SUCCESS);

            ASSERT(target_thread->IsWaitingForAddressArbiter());
            target_thread->Wakeup();

            it = thread_tree.erase(it);
            target_thread->ClearAddressArbiter();
            ++num_waiters;
        }
    }
    return RESULT_SUCCESS;
}

ResultCode KAddressArbiter::SignalAndModifyByWaitingCountIfEqual(VAddr addr, s32 value, s32 count) {
    // Perform signaling.
    s32 num_waiters{};
    {
        KScopedSchedulerLock sl(kernel);

        auto it = thread_tree.nfind_light({addr, -1});
        // Determine the updated value.
        s32 new_value{};
        if (/*GetTargetFirmware() >= TargetFirmware_7_0_0*/ true) {
            if (count <= 0) {
                if ((it != thread_tree.end()) && (it->GetAddressArbiterKey() == addr)) {
                    new_value = value - 2;
                } else {
                    new_value = value + 1;
                }
            } else {
                if ((it != thread_tree.end()) && (it->GetAddressArbiterKey() == addr)) {
                    auto tmp_it = it;
                    s32 tmp_num_waiters{};
                    while ((++tmp_it != thread_tree.end()) &&
                           (tmp_it->GetAddressArbiterKey() == addr)) {
                        if ((tmp_num_waiters++) >= count) {
                            break;
                        }
                    }

                    if (tmp_num_waiters < count) {
                        new_value = value - 1;
                    } else {
                        new_value = value;
                    }
                } else {
                    new_value = value + 1;
                }
            }
        } else {
            if (count <= 0) {
                if ((it != thread_tree.end()) && (it->GetAddressArbiterKey() == addr)) {
                    new_value = value - 1;
                } else {
                    new_value = value + 1;
                }
            } else {
                auto tmp_it = it;
                s32 tmp_num_waiters{};
                while ((tmp_it != thread_tree.end()) && (tmp_it->GetAddressArbiterKey() == addr) &&
                       (tmp_num_waiters < count + 1)) {
                    ++tmp_num_waiters;
                    ++tmp_it;
                }

                if (tmp_num_waiters == 0) {
                    new_value = value + 1;
                } else if (tmp_num_waiters <= count) {
                    new_value = value - 1;
                } else {
                    new_value = value;
                }
            }
        }

        // Check the userspace value.
        s32 user_value{};
        bool succeeded{};
        if (value != new_value) {
            succeeded = UpdateIfEqual(system, std::addressof(user_value), addr, value, new_value);
        } else {
            succeeded = ReadFromUser(system, std::addressof(user_value), addr);
        }

        R_UNLESS(succeeded, Svc::ResultInvalidCurrentMemory);

        if (user_value != value) {
            return Svc::ResultInvalidState;
        }

        while ((it != thread_tree.end()) && (count <= 0 || num_waiters < count) &&
               (it->GetAddressArbiterKey() == addr)) {
            KThread* target_thread = std::addressof(*it);
            target_thread->SetSyncedObject(nullptr, RESULT_SUCCESS);

            ASSERT(target_thread->IsWaitingForAddressArbiter());
            target_thread->Wakeup();

            it = thread_tree.erase(it);
            target_thread->ClearAddressArbiter();
            ++num_waiters;
        }
    }
    return RESULT_SUCCESS;
}

ResultCode KAddressArbiter::WaitIfLessThan(VAddr addr, s32 value, bool decrement, s64 timeout) {
    // Prepare to wait.
    KThread* cur_thread = kernel.CurrentScheduler()->GetCurrentThread();

    {
        KScopedSchedulerLockAndSleep slp{kernel, cur_thread, timeout};

        // Check that the thread isn't terminating.
        if (cur_thread->IsTerminationRequested()) {
            slp.CancelSleep();
            return Svc::ResultTerminationRequested;
        }

        // Set the synced object.
        cur_thread->SetSyncedObject(nullptr, Svc::ResultTimedOut);

        // Read the value from userspace.
        s32 user_value{};
        bool succeeded{};
        if (decrement) {
            succeeded = DecrementIfLessThan(system, std::addressof(user_value), addr, value);
        } else {
            succeeded = ReadFromUser(system, std::addressof(user_value), addr);
        }

        if (!succeeded) {
            slp.CancelSleep();
            return Svc::ResultInvalidCurrentMemory;
        }

        // Check that the value is less than the specified one.
        if (user_value >= value) {
            slp.CancelSleep();
            return Svc::ResultInvalidState;
        }

        // Check that the timeout is non-zero.
        if (timeout == 0) {
            slp.CancelSleep();
            return Svc::ResultTimedOut;
        }

        // Set the arbiter.
        cur_thread->SetAddressArbiter(std::addressof(thread_tree), addr);
        thread_tree.insert(*cur_thread);
        cur_thread->SetState(ThreadState::Waiting);
        cur_thread->SetWaitReasonForDebugging(ThreadWaitReasonForDebugging::Arbitration);
    }

    // Cancel the timer wait.
    kernel.TimeManager().UnscheduleTimeEvent(cur_thread);

    // Remove from the address arbiter.
    {
        KScopedSchedulerLock sl(kernel);

        if (cur_thread->IsWaitingForAddressArbiter()) {
            thread_tree.erase(thread_tree.iterator_to(*cur_thread));
            cur_thread->ClearAddressArbiter();
        }
    }

    // Get the result.
    KSynchronizationObject* dummy{};
    return cur_thread->GetWaitResult(std::addressof(dummy));
}

ResultCode KAddressArbiter::WaitIfEqual(VAddr addr, s32 value, s64 timeout) {
    // Prepare to wait.
    KThread* cur_thread = kernel.CurrentScheduler()->GetCurrentThread();

    {
        KScopedSchedulerLockAndSleep slp{kernel, cur_thread, timeout};

        // Check that the thread isn't terminating.
        if (cur_thread->IsTerminationRequested()) {
            slp.CancelSleep();
            return Svc::ResultTerminationRequested;
        }

        // Set the synced object.
        cur_thread->SetSyncedObject(nullptr, Svc::ResultTimedOut);

        // Read the value from userspace.
        s32 user_value{};
        if (!ReadFromUser(system, std::addressof(user_value), addr)) {
            slp.CancelSleep();
            return Svc::ResultInvalidCurrentMemory;
        }

        // Check that the value is equal.
        if (value != user_value) {
            slp.CancelSleep();
            return Svc::ResultInvalidState;
        }

        // Check that the timeout is non-zero.
        if (timeout == 0) {
            slp.CancelSleep();
            return Svc::ResultTimedOut;
        }

        // Set the arbiter.
        cur_thread->SetAddressArbiter(std::addressof(thread_tree), addr);
        thread_tree.insert(*cur_thread);
        cur_thread->SetState(ThreadState::Waiting);
        cur_thread->SetWaitReasonForDebugging(ThreadWaitReasonForDebugging::Arbitration);
    }

    // Cancel the timer wait.
    kernel.TimeManager().UnscheduleTimeEvent(cur_thread);

    // Remove from the address arbiter.
    {
        KScopedSchedulerLock sl(kernel);

        if (cur_thread->IsWaitingForAddressArbiter()) {
            thread_tree.erase(thread_tree.iterator_to(*cur_thread));
            cur_thread->ClearAddressArbiter();
        }
    }

    // Get the result.
    KSynchronizationObject* dummy{};
    return cur_thread->GetWaitResult(std::addressof(dummy));
}

} // namespace Kernel
