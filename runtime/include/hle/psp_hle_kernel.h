#pragma once
#include <cstdint>
#include <chrono>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <memory>

// ---- Thread UID Tracking ----
struct PspThreadInfo {
    int uid;           // PSP-visible UID (0x100+)
    int thid;          // Scheduler slot index
    uint32_t entry_addr;
    uint32_t stack_base;
    uint32_t stack_size;
    int priority;
    char name[32];
};

// ---- Semaphore ----

/// FIFO wait-queue entry for sceKernelWaitSema. Stack-allocated by the
/// waiting thread; the signaller transfers the count directly to the
/// waiter (granted=true) so polling threads cannot steal it (issue #29).
struct SemaWaiter {
    int need;            // signal count this waiter requires
    bool granted;        // set true by SignalSema after count transfer
};

struct PspSemaphore {
    int uid;
    char name[32];
    int current_count;
    int max_count;
    int init_count;
    int wait_count = 0;
    bool deleted = false;
    std::deque<SemaWaiter*> waiters;  // FIFO; guarded by mtx
    std::chrono::steady_clock::time_point last_stuck_warn{};
    std::mutex mtx;
    std::condition_variable cv;
};

// ---- Mutex ----
struct PspMutex {
    int uid;
    char name[32];
    int lock_count;    // for recursive mutexes
    int owner_thid;    // -1 if unlocked
    std::mutex mtx;
    std::condition_variable cv;
};

// ---- Event Flag ----
struct PspEventFlag {
    int uid;
    char name[32];
    uint32_t pattern;  // current bit pattern
    std::mutex mtx;
    std::condition_variable cv;
};

// ---- Callback ----
struct PspCallback {
    int uid;
    char name[32];
    uint32_t func_addr;   // Address of recompiled callback function
    uint32_t user_arg;    // User argument passed to sceKernelCreateCallback
    bool pending;         // Set true when notified, cleared after dispatch
    int notify_count;     // Incremented each notification
    int notify_arg;       // Argument from notification (e.g., async IO result)
};

/// Mark a callback as pending so sceKernelCheckCallback will dispatch it.
void psp_kernel_notify_callback(int cbId, int notifyArg);

/// Dispatch one pending callback (if any). Called by *CB HLE variants.
void psp_kernel_check_callbacks(uint8_t* rdram, recomp_context* ctx);

// Event flag wait modes
constexpr uint32_t PSP_EVENT_WAITOR   = 0x00;
constexpr uint32_t PSP_EVENT_WAITAND  = 0x01;
constexpr uint32_t PSP_EVENT_WAITCLEAR = 0x20;
constexpr uint32_t PSP_EVENT_WAITCLEARALL = 0x10;

// ---- UID Generator ----
int psp_next_uid();

// ---- Stack Allocator ----
// Allocates guest stack from top of PSP user memory downward.
uint32_t psp_alloc_stack(uint8_t* rdram, uint32_t size);

// ---- Workaround API ----
// Signal a semaphore by UID without going through HLE argument parsing.
void psp_hle_signal_sema_by_uid(int uid, int count);

// ---- Registration Functions ----
void psp_hle_register_kernel_thread();
void psp_hle_register_kernel_memory();
void psp_hle_register_kernel_sema();
void psp_hle_register_kernel_mutex();
void psp_hle_register_kernel_eventflag();
