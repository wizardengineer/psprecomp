#include "hle/psp_hle.h"
#include "hle/psp_hle_kernel.h"
#include "psp_scheduler.h"
#include "psp_memory.h"
#include "recomp.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <memory>

static std::unordered_map<int, std::unique_ptr<PspMutex>> g_mutexes;

// ---- HLE Functions ----

static void hle_sceKernelCreateMutex(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t name_ptr = static_cast<uint32_t>(ctx->r[4]);
    int32_t attr = ctx->r[5];
    int32_t init_count = ctx->r[6];

    (void)attr;

    auto mtx = std::make_unique<PspMutex>();
    mtx->uid = psp_next_uid();
    mtx->lock_count = init_count;
    mtx->owner_thid = (init_count > 0) ? 0 : -1;
    std::memset(mtx->name, 0, sizeof(mtx->name));

    if (name_ptr != 0) {
        const char* name = reinterpret_cast<const char*>(
            rdram + (name_ptr & PSP_ADDR_MASK));
        std::strncpy(mtx->name, name, sizeof(mtx->name) - 1);
    }

    int uid = mtx->uid;
    g_mutexes[uid] = std::move(mtx);

    ctx->r[2] = uid;
}

static void hle_sceKernelDeleteMutex(
    uint8_t* rdram, recomp_context* ctx
) {
    int uid = ctx->r[4];
    g_mutexes.erase(uid);
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelLockMutex(
    uint8_t* rdram, recomp_context* ctx
) {
    int uid = ctx->r[4];
    int32_t count = ctx->r[5];

    // [F2] OFF: leading cooperative yield (unchanged, byte-identical). ON: skip
    // it — release_for_wait below is the sole hand-off for the blocking path.
    if (!sched_token_enabled()) {
        sched_yield_point();
    }

    auto it = g_mutexes.find(uid);
    if (it == g_mutexes.end()) {
        ctx->r[2] = SCE_KERNEL_ERROR_MUTEX_NOT_FOUND;
        (void)rdram;
        return;
    }

    auto& m = it->second;
    std::unique_lock<std::mutex> lock(m->mtx);

    PspThread* t = psp_get_current_thread();
    int my_thid = t ? t->id : 0;

    // [M1.b] Wire the run-token AND add a finite valve. The old wait was an
    // UNBOUNDED m->cv.wait(pred) — no g_should_exit escape, no token_parked, so
    // under the flag it held the token while asleep on a foreign cv (lost-token
    // risk) and could hang forever on a missed notify. Replace with a finite
    // 5s-valve loop bracketed by the token helpers. The valve only ADDS a wake;
    // correctness on a real signal is unchanged (UnlockMutex still notify_one's).
    // sched_token_* are no-ops when OFF; OFF behavior changes ONLY in the
    // pathological missed-notify/shutdown case (previously a permanent hang).
    auto acquired = [&] {
        return m->owner_thid == -1 || m->owner_thid == my_thid;
    };
    // [F1] Un-nest the blocking token re-acquire from under m->mtx. Park on the
    // cv under m->mtx (as before), but reacquire the token with m->mtx DROPPED
    // (sched_token_reacquire_unlocked: unlock -> token_acquire -> re-lock), then
    // re-check acquired() under the re-taken m->mtx — the owner may have changed
    // while m->mtx was dropped. Without this, a contender parked in token_acquire
    // would strand m->mtx and UnlockMutex would block on it (AB-BA livelock). The
    // 5s valve is unchanged. OFF: sched_token_reacquire_unlocked is a no-op, so
    // m->mtx is held across exactly the original cv-wait loop and the reacquire
    // adds nothing (byte-identical; only the pathological missed-notify case,
    // already covered by the existing valve, differs).
    sched_token_release_for_wait();
    while (!acquired() && !g_should_exit.load()) {
        m->cv.wait_for(lock, std::chrono::seconds(5),
            [&] { return acquired() || g_should_exit.load(); });
    }
    sched_token_reacquire_unlocked(lock);  // [F1] re-acquire with m->mtx dropped
    // [F1] Re-validate under the re-taken m->mtx: the owner may have flipped while
    // m->mtx was dropped during the token re-acquire. If we lost the race, re-park
    // (ON only — OFF never released the token, so acquired() is already final and
    // this guard re-checks the same continuously-locked predicate ⇒ no extra
    // iteration OFF, byte-identical).
    while (sched_token_enabled() && !acquired() && !g_should_exit.load()) {
        sched_token_release_for_wait();
        m->cv.wait_for(lock, std::chrono::seconds(5),
            [&] { return acquired() || g_should_exit.load(); });
        sched_token_reacquire_unlocked(lock);
    }

    m->owner_thid = my_thid;
    m->lock_count += count;
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelLockMutexCB(
    uint8_t* rdram, recomp_context* ctx
) {
    psp_kernel_check_callbacks(rdram, ctx);
    hle_sceKernelLockMutex(rdram, ctx);
}

static void hle_sceKernelUnlockMutex(
    uint8_t* rdram, recomp_context* ctx
) {
    int uid = ctx->r[4];
    int32_t count = ctx->r[5];

    auto it = g_mutexes.find(uid);
    if (it == g_mutexes.end()) {
        ctx->r[2] = SCE_KERNEL_ERROR_MUTEX_NOT_FOUND;
        (void)rdram;
        return;
    }

    auto& m = it->second;
    std::unique_lock<std::mutex> lock(m->mtx);

    m->lock_count -= count;
    if (m->lock_count <= 0) {
        m->lock_count = 0;
        m->owner_thid = -1;
        m->cv.notify_one();
    }
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelTryLockMutex(
    uint8_t* rdram, recomp_context* ctx
) {
    int uid = ctx->r[4];
    int32_t count = ctx->r[5];

    auto it = g_mutexes.find(uid);
    if (it == g_mutexes.end()) {
        ctx->r[2] = SCE_KERNEL_ERROR_MUTEX_NOT_FOUND;
        (void)rdram;
        return;
    }

    auto& m = it->second;
    std::unique_lock<std::mutex> lock(m->mtx);

    PspThread* t = psp_get_current_thread();
    int my_thid = t ? t->id : 0;

    if (m->owner_thid == -1 || m->owner_thid == my_thid) {
        m->owner_thid = my_thid;
        m->lock_count += count;
        ctx->r[2] = SCE_OK;
    } else {
        ctx->r[2] = SCE_KERNEL_ERROR_WAIT_TIMEOUT;
    }
    (void)rdram;
}

// ---- Registration ----

void psp_hle_register_kernel_mutex() {
    psp_hle_register("sceKernelCreateMutex",
                      hle_sceKernelCreateMutex);
    psp_hle_register("sceKernelDeleteMutex",
                      hle_sceKernelDeleteMutex);
    psp_hle_register("sceKernelLockMutex",
                      hle_sceKernelLockMutex);
    psp_hle_register("sceKernelLockMutexCB",
                      hle_sceKernelLockMutexCB);
    psp_hle_register("sceKernelUnlockMutex",
                      hle_sceKernelUnlockMutex);
    psp_hle_register("sceKernelTryLockMutex",
                      hle_sceKernelTryLockMutex);
}
