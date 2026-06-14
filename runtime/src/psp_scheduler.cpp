#include "psp_scheduler.h"
#include "psp_debug_socket.h"  // PspDebugThreadInfo ([#35] I command)
#include "psp_vfpu.h"  // vfpu_init_context — VFPU prefix reset default
#include "hle/psp_hle.h"  // SCE_KERNEL_ERROR_WAIT_TIMEOUT
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/// Global scheduler lock — held during ALL status transitions and cv ops.
static std::mutex g_sched_mutex;

/// Thread pool — 64 slots matching PSP kernel limits.
static PspThread g_threads[MAX_THREADS];

/// Thread-local pointer to this OS thread's PspThread.
/// Each OS thread knows its own PspThread without shared state lookup.
static thread_local PspThread* g_current = nullptr;

// ===========================================================================
// Single-runnable token (approach (d), SCHEDULER-DESIGN.md §4(d)) — FLAG-GATED.
//
// When PSPRECOMP_PREEMPT=1, exactly one PSP thread may execute guest code at a
// time, restoring PSP single-CPU interleaving + ordered cross-thread hand-off
// (the .hack CRI-ring fix). When the flag is OFF (default) NONE of this is
// engaged: token_acquire/token_release_to are no-ops and the scheduler behaves
// byte-identically to the pre-token parallel model (the Patapon-default gate).
//
// "Hold the token" == g_token_holder == self. A thread acquires by parking on
// its own cv until granted; a thread releases by SELECTING a runnable peer and
// transferring the token to it (deadlock-free: a blocking/yielding holder always
// hands the token to a selected peer BEFORE it waits). If no peer is runnable,
// the holder keeps the token (never release into the void).
// ===========================================================================

/// Current token holder, guarded by g_sched_mutex. nullptr == free (only at
/// startup before the first thread acquires, or after the last thread exits).
static PspThread* g_token_holder = nullptr;

/// Read once, cached. Mirrors sched_preempt_enabled() — declared here so token
/// helpers can gate on it; the canonical reader is sched_preempt_enabled below.
static bool token_enabled();

/// Within-priority round-robin cursor: index after the last thread that was
/// granted the token, so equal-priority peers take turns (PPSSPP pop_first).
/// [F2-core] Retained as the SECONDARY within-priority tiebreak (stable,
/// deterministic) behind the primary "least-recently-ran" key — see
/// select_next_runnable. Only updated on the ON path (token_release_to).
static int g_rr_cursor = 0;

/// [F2-core] Monotonic grant sequence, guarded by g_sched_mutex. Bumped every
/// time a thread is GRANTED the token (becomes g_token_holder), and the grantee's
/// PspThread::token_last_ran is set to the new value. select_next_runnable uses
/// token_last_ran as the within-priority tiebreak (smallest = least-recently-ran
/// = picked first), so a just-yielded holder is demoted below a starved peer.
/// ON-only: every write/read is on a path that early-returns at the OFF
/// token_enabled() gate, so OFF this counter never moves (stays 0) and no thread's
/// token_last_ran is ever touched — OFF byte-identical.
static uint64_t g_token_grant_seq = 0;

/// [F2-core] Stamp `t` as just-granted: bump the global grant sequence and record
/// it on the grantee. Caller holds g_sched_mutex AND must be on the ON path (every
/// call site is already gated by token_enabled()). Never called OFF.
static void token_mark_granted(PspThread* t) {
    g_token_grant_seq++;
    t->token_last_ran = g_token_grant_seq;
}

/// [H2] Promote a token grantee to RUNNING. A thread that HOLDS the run-token IS
/// running guest code, so the grant must transition the grantee out of READY —
/// otherwise a freshly-started producer parked in thread_entry_wrapper on the
/// `status == RUNNING` predicate treats the grant's cv.notify as a SPURIOUS wake
/// (predicate still false), re-sleeps, and STRANDS the token: it runs zero guest
/// instructions until the 50ms force-run valve fires (the .hack CRI-producer
/// stranding, sched-d-dothack-converge.md §1/§4 H2). select_next_runnable only
/// ever grants to a READY or RUNNING thread (WAIT/WAIT_SLEEP/DORMANT are excluded
/// from grantee eligibility), so this promotion only ever touches READY -> RUNNING
/// and is idempotent for an already-RUNNING holder. Caller holds g_sched_mutex AND
/// is on the ON path (every call site is gated by token_enabled()); never OFF, so
/// OFF status transitions are byte-identical. WAIT/WAIT_SLEEP/DORMANT are left
/// untouched defensively (must never reach here — see select_next_runnable).
static void token_promote_to_running(PspThread* t) {
    if (t->status == READY) {
        t->status = RUNNING;
    }
}

/// [approach (d) perf pass, M5/F1] ON-only diagnostic counters for the token
/// wait valve. Incremented ONLY inside token_acquire, which returns at its OFF
/// gate before reaching them, so OFF never touches these (OFF byte-identical).
///   g_valve_timeout_wakes  : wait_for returned false (50ms backstop fired with
///                            no predicate) — steady-state ~0 expected.
///   g_valve_nullptr_claims : the FREE-accept branch claimed a void-released
///                            token (R2) — small-and-bounded expected; a high or
///                            growing rate signals a missed direct-grant.
static std::atomic<uint64_t> g_valve_timeout_wakes{0};
static std::atomic<uint64_t> g_valve_nullptr_claims{0};

/// Select the next thread to receive the token: highest-priority READY/RUNNING
/// thread (lowest priority number), round-robin within a priority, excluding
/// `exclude`. Returns nullptr if none runnable. Caller holds g_sched_mutex.
///
/// [approach (d) perf pass — M1 INVARIANT] select_next_runnable excludes every
/// token_parked thread; this is sound ONLY because every object-cv / busy-poll
/// park that can block a PSP thread (eventflag, sema slow/CB/deleted, mutex,
/// lwmutex, SendMsgPipe-full, and the render-queue GE/present round-trips on a
/// PSP thread) brackets its wait with sched_token_release_for_wait() /
/// sched_token_reacquire_after_wait(), holding token_parked for the entire park.
/// A token_parked thread is NEVER g_token_holder — the three scheduler park
/// sites (release_for_wait, sleep_current, wait_end) void-release the token
/// (g_token_holder = nullptr) BEFORE parking. Render-thread cvs and real-time
/// host sleeps (DelayThread, audio, vblank) are intentionally NOT bracketed —
/// they are not PSP-thread peer parks. The debug-assert below enforces the
/// holder/parked exclusivity (compiles out in NDEBUG; the body is unreachable
/// OFF since g_token_holder stays nullptr).
static PspThread* select_next_runnable(PspThread* exclude) {
    assert(!(g_token_holder && g_token_holder->token_parked) &&
           "INVARIANT: a token_parked thread is never g_token_holder");
    // [F2-core] Within-priority tiebreak is "least-recently-ran" (smallest
    // token_last_ran) so a just-yielded holder is demoted and a starved /
    // freshly-un-parked peer is preferred — fixing the hand-off routing
    // starvation. g_rr_cursor remains the SECONDARY tiebreak (stable rotation
    // among equal-priority peers that share a token_last_ran, e.g. both 0 at
    // startup), preserving determinism. This whole function is only reachable on
    // the ON path (every caller — token_release_to, other_runnable_exists,
    // token_acquire — early-returns at the token_enabled() gate before calling
    // it), so OFF it is never entered and token_last_ran is never read; the
    // tiebreak change is OFF-invisible by construction.
    PspThread* best = nullptr;
    // Scan starting just after the RR cursor so equal-priority peers with an
    // equal token_last_ran still rotate (secondary tiebreak / determinism).
    for (int n = 0; n < MAX_THREADS; n++) {
        int i = (g_rr_cursor + 1 + n) % MAX_THREADS;
        PspThread& t = g_threads[i];
        if (!t.in_use || &t == exclude) {
            continue;
        }
        // Runnable == will execute guest code: READY or RUNNING. Exclude
        // token_parked threads — they released the token and are blocked on a
        // foreign object cv (sema/eventflag) or sleeping; granting the token to
        // them would lose it (they will not wake to run guest code). A thread
        // parked in token_acquire (token_parked stays false there) IS eligible:
        // it parks on its own cv and the grant notify wakes it.
        if (t.token_parked) {
            continue;
        }
        if (t.status != READY && t.status != RUNNING) {
            continue;
        }
        if (!best) {
            best = &t;
            continue;
        }
        // Primary key: highest priority (lowest number) first.
        if (t.priority < best->priority) {
            best = &t;
            continue;
        }
        // Within the same priority: least-recently-ran (smallest token_last_ran)
        // wins. Strict `<` keeps the RR-cursor-ordered first match on ties, so
        // equal-stamp peers (e.g. startup 0s) still rotate via g_rr_cursor.
        if (t.priority == best->priority &&
            t.token_last_ran < best->token_last_ran) {
            best = &t;
        }
    }
    return best;
}

/// Transfer the token from the current holder to a selected runnable peer and
/// wake it. Caller holds g_sched_mutex. No-op when the flag is OFF. If no peer
/// is runnable the holder keeps the token (the caller must then NOT block, or
/// will park harmlessly on its own cv with the 50ms safety valve).
static void token_release_to(PspThread* self) {
    if (!token_enabled()) {
        return;
    }
    PspThread* next = select_next_runnable(self);
    if (!next) {
        return;  // No runnable peer — keep the token (don't release into void).
    }
    g_token_holder = next;
    token_mark_granted(next);  // [F2-core] stamp the grantee least-recently-ran
    token_promote_to_running(next);  // [H2] grantee IS now running guest code
    for (int i = 0; i < MAX_THREADS; i++) {
        if (&g_threads[i] == next) {
            g_rr_cursor = i;
            break;
        }
    }
    next->cv.notify_one();
}

/// Park on self's cv until self holds the token (or shutdown). Caller holds
/// g_sched_mutex via `lock`. No-op when the flag is OFF. Used after a release
/// to re-contend, and at first run to take the initial token.
static void token_acquire(std::unique_lock<std::mutex>& lock, PspThread* self) {
    if (!token_enabled()) {
        return;
    }
    // [M2] Clear token_parked FIRST — before the first-free check and the wait
    // loop. A thread re-contending for the token (via reacquire_after_wait, or
    // the sleep/wait-end tails) is no longer parked on a FOREIGN object cv; it
    // is now waiting on its OWN cv here, which is exactly a valid grantee. M2
    // makes it immediately eligible so select_next_runnable / other_runnable_
    // exists can grant+notify it directly, instead of leaving it invisible until
    // the old after-loop clear (which forced the .hack producer's wakeup onto
    // the 50ms valve — the perf collapse).
    self->token_parked = false;
    // [M2 guard] Load-bearing: if a grant was delivered to self during the
    // thread_entry startup window (or any prior race) self may already hold the
    // token — recover it cheaply, nothing to do. This is NOT the dropped M3
    // keep-token path; it is a correctness guard for an already-granted holder.
    if (g_token_holder == self) {
        return;
    }
    // First-free fast path: if the token is free, take it directly.
    if (g_token_holder == nullptr) {
        g_token_holder = self;
        token_mark_granted(self);  // [F2-core] stamp self as just-ran
        token_promote_to_running(self);  // [H2] holder IS running guest code
        return;
    }
    // [M5/F1] Predicated wait — no missed wake. The predicate also accepts a
    // FREE token (R2): R1's void-release can leave g_token_holder == nullptr, so
    // a peer parked here must be able to claim it. The in-body nullptr-claim is
    // KEPT and is serialized by g_sched_mutex + the while re-check ⇒ exactly one
    // claimant (two peers woken by the same void-release re-take the lock
    // serially: the first sets g_token_holder = self and exits; the second
    // re-checks the while condition, sees the holder is now the first peer, and
    // re-parks). 50ms (SCHED_TIMEOUT_MS) is a pure backstop now (fires ~never).
    while (g_token_holder != self && !g_should_exit.load()) {
        bool woke = self->cv.wait_for(
            lock,
            std::chrono::milliseconds(SCHED_TIMEOUT_MS),
            [&] {
                return g_token_holder == self        // granted to me
                    || g_token_holder == nullptr     // R2: freed — claimable
                    || g_should_exit.load();
            });
        if (!woke) {
            ++g_valve_timeout_wakes;  // 50ms backstop fired without predicate
        }
        if (g_token_holder == nullptr) {  // R2: claim the freed token
            g_token_holder = self;
            token_mark_granted(self);  // [F2-core] stamp self as just-ran
            token_promote_to_running(self);  // [H2] holder IS running guest code
            ++g_valve_nullptr_claims;
        }
    }
}

/// [F3] Is there ANY other runnable thread that could take the token? Reuses
/// the EXACT eligibility predicate (excludes self, every token_parked thread,
/// and any non-READY/RUNNING thread), so a parked producer never counts as
/// "another runnable" — which is precisely why M1 (every object-cv park sets
/// token_parked) is the precondition for this being safe. Caller holds
/// g_sched_mutex.
static bool other_runnable_exists(PspThread* self) {
    return select_next_runnable(self) != nullptr;
}

/// Release the token held by `self` to a peer, then re-acquire (block until
/// granted again). The unified hand-off used at every voluntary reschedule
/// point. Caller holds g_sched_mutex via `lock`. No-op when the flag is OFF.
/// This is the ONLY symmetric keep-token site (the holder stays RUNNING — it
/// never blocks, it just yields); the three R1 park sites always void-release.
static void token_handoff(std::unique_lock<std::mutex>& lock, PspThread* self) {
    if (!token_enabled() || g_token_holder != self) {
        return;
    }
    // [F3] Sole-runnable fast path: if no other thread can take the token, keep
    // it and continue — skip the release/reacquire round trip entirely (zero cv
    // ops, one 64-slot scan). Safe because M1 makes every object-cv-parked peer
    // token_parked (invisible to the scan) and M2 makes a re-contending peer
    // visible the instant it re-enters token_acquire, so the holder hands off on
    // its NEXT back-edge once a peer actually un-parks.
    if (!other_runnable_exists(self)) {
        return;
    }
    token_release_to(self);
    if (g_token_holder == self) {
        return;  // Kept it (no runnable peer) — no need to park.
    }
    token_acquire(lock, self);
}

// ---------------------------------------------------------------------------
// psp_scheduler_init — zero all 64 slots
// ---------------------------------------------------------------------------
void psp_scheduler_init() {
    std::unique_lock<std::mutex> lock(g_sched_mutex);
    for (int i = 0; i < MAX_THREADS; i++) {
        g_threads[i].id = i;
        g_threads[i].in_use = false;
        g_threads[i].priority = 32;
        g_threads[i].status = DORMANT;
        g_threads[i].rdram = nullptr;
        g_threads[i].entry_addr = 0;
        g_threads[i].arg_value = 0;
        g_threads[i].stack_top = 0;
        g_threads[i].wakeup_count = 0;
        std::memset(g_threads[i].name, 0, sizeof(g_threads[i].name));
        std::memset(g_threads[i].wait_reason, 0,
                    sizeof(g_threads[i].wait_reason));
        std::memset(&g_threads[i].ctx, 0, sizeof(recomp_context));
        vfpu_init_context(&g_threads[i].ctx);
    }
}

// ---------------------------------------------------------------------------
// psp_thread_create — allocate slot, set DORMANT
// ---------------------------------------------------------------------------
int psp_thread_create(
    const char* name,
    uint32_t entry,
    int priority,
    uint32_t stack_top,
    uint32_t arg
) {
    std::unique_lock<std::mutex> lock(g_sched_mutex);
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!g_threads[i].in_use) {
            PspThread& t = g_threads[i];
            t.in_use = true;
            t.priority = priority;
            t.status = DORMANT;
            t.entry_addr = entry;
            t.arg_value = arg;
            t.stack_top = stack_top;
            t.wakeup_count = 0;
            std::memset(t.name, 0, sizeof(t.name));
            std::memset(t.wait_reason, 0, sizeof(t.wait_reason));
            if (name) {
                std::strncpy(t.name, name, sizeof(t.name) - 1);
            }
            std::memset(&t.ctx, 0, sizeof(recomp_context));
            vfpu_init_context(&t.ctx);
            return i;
        }
    }
    return -1; // All slots full
}

// ---------------------------------------------------------------------------
// Forward declaration — thread entry wrapper
// ---------------------------------------------------------------------------
static void thread_entry_wrapper(PspThread* t);

// ---------------------------------------------------------------------------
// psp_thread_start — set READY, launch OS thread
// ---------------------------------------------------------------------------
int psp_thread_start(int thid) {
    std::unique_lock<std::mutex> lock(g_sched_mutex);
    if (thid < 0 || thid >= MAX_THREADS) {
        return -1;
    }
    PspThread& t = g_threads[thid];
    if (!t.in_use || t.status != DORMANT) {
        return -1;
    }
    t.status = READY;
    g_alive_threads.fetch_add(1);
    t.host_thread = std::thread(thread_entry_wrapper, &t);
    return 0;
}

// ---------------------------------------------------------------------------
// thread_entry_wrapper — OS thread entry point
// ---------------------------------------------------------------------------
static void thread_entry_wrapper(PspThread* t) {
    g_current = t;

    // Wait until this thread is scheduled to RUNNING (or exit requested)
    {
        std::unique_lock<std::mutex> lock(g_sched_mutex);
        t->cv.wait_for(
            lock,
            std::chrono::milliseconds(SCHED_TIMEOUT_MS),
            [&] {
                return t->status == RUNNING ||
                       g_should_exit.load();
            }
        );
        // Safety valve: if timed out and still READY, force-run
        if (t->status != RUNNING && !g_should_exit.load()) {
            t->status = RUNNING;
        }
    }

    // Exit early if shutdown requested
    if (g_should_exit.load()) {
        g_alive_threads.fetch_sub(1);
        std::unique_lock<std::mutex> lock(g_sched_mutex);
        t->status = DEAD;
        t->cv.notify_all();
        return;
    }

    // Register context (SP, k0, args) set by hle_sceKernelStartThread
    // before psp_thread_start() was called. Do not reinitialize here.

    // Single-runnable token (flag-gated, no-op when OFF): a PSP thread must hold
    // the token to execute guest code. Acquire it before entering guest code;
    // every yield/block/exit hands it off, every resume re-acquires.
    {
        std::unique_lock<std::mutex> lock(g_sched_mutex);
        token_acquire(lock, t);
    }

    // Call the recompiled PSP entry function via dispatch table.
    // Catch PspThreadExitException -- thrown by sceKernelExitThread
    // to implement "never returns" semantics. On the real PSP,
    // ExitThread terminates the thread immediately; we use C++
    // exception unwinding to achieve the same effect.
    try {
        FuncPtr entry = RECOMP_LOOKUP(t->entry_addr);
        if (entry) {
            entry(t->rdram, &t->ctx);
        }
    } catch (const PspThreadExitException& ex) {
        std::fprintf(stderr,
            "[SCHED] Thread %d (\"%s\") exited via "
            "sceKernelExitThread(status=%d)\n",
            t->id, t->name, ex.status);
    }

    // Thread function returned (or was exited) — mark dead
    std::fprintf(stderr,
        "[SCHED] Thread %d (\"%s\") entry function returned, marking dead "
        "(alive=%d)\n",
        t->id, t->name, g_alive_threads.load());
    psp_thread_exit_current();
}

// ---------------------------------------------------------------------------
// sched_yield_point — cooperative yield (RUNTIME-05, RUNTIME-06)
// ---------------------------------------------------------------------------
void sched_yield_point() {
    if (g_should_exit.load()) {
        return;
    }
    if (!g_current) {
        return;
    }

    std::unique_lock<std::mutex> lock(g_sched_mutex);

    // Single-runnable path (flag ON): the unified token hand-off. Release the
    // token to the selected runnable peer (priority + within-priority RR) and
    // block until re-granted. Deadlock-free: token_handoff selects+grants BEFORE
    // it parks, and keeps the token if no peer is runnable.
    if (token_enabled()) {
        token_handoff(lock, g_current);
        return;
    }

    // ---- Default parallel path (flag OFF) — byte-identical to pre-token ----
    // Find highest-priority READY thread (lowest priority number)
    PspThread* best = nullptr;
    for (int i = 0; i < MAX_THREADS; i++) {
        PspThread& t = g_threads[i];
        if (!t.in_use || t.status != READY) {
            continue;
        }
        if (&t == g_current) {
            continue;
        }
        if (!best || t.priority < best->priority) {
            best = &t;
        }
    }

    // No switch needed if no READY thread or current is higher priority
    if (!best || g_current->priority <= best->priority) {
        return;
    }

    // Context switch: yield to best, suspend current
    PspThread* current = g_current;
    if (current->status == RUNNING) {
        current->status = READY;
    }
    best->status = RUNNING;

    // Wake the best thread
    best->cv.notify_one();

    // Wait until we are re-scheduled (50ms safety valve)
    current->cv.wait_for(
        lock,
        std::chrono::milliseconds(SCHED_TIMEOUT_MS),
        [&] {
            return current->status == RUNNING ||
                   current->status == DEAD ||
                   g_should_exit.load();
        }
    );

    // Safety valve: if timed out and still READY, force-resume
    if (current->status == READY) {
        current->status = RUNNING;
    }
}

// ---------------------------------------------------------------------------
// sched_preempt — instruction-budget preemption hook (#66, approach (a))
//
// Called from emitted code at loop back-edges once ctx->preempt_budget hits
// <= 0. DEFAULT-OFF: with PSPRECOMP_PREEMPT unset/"0" this is a no-op beyond
// reloading the budget — the back-edge decrement becomes a dead effect and
// guest behavior is byte-identical to the pre-#66 scheduler (the gate this unit
// must pass on Patapon). When PSPRECOMP_PREEMPT=1 it additionally takes a fair
// cooperative yield so a syscall-free busy-poll reaches a reschedule point;
// step 3 (#66) replaces this with the real preemptive yield and validates it
// against the .hack freeze-rate harness. The flag is read once and cached.
// ---------------------------------------------------------------------------
static bool sched_preempt_enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("PSPRECOMP_PREEMPT");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    return enabled;
}

/// Token gate uses the same flag (read once, cached) as the preempt hook.
static bool token_enabled() {
    return sched_preempt_enabled();
}

/// [M4] Public face of the cached token gate — one source of truth. HLE wait
/// stubs use this to gate ON-only timeout cadences and skip the redundant
/// leading sched_yield_point() before an object-cv park (F2). OFF returns false,
/// so every caller takes its OFF/legacy branch (byte-identical default path).
bool sched_token_enabled() {
    return token_enabled();
}

void sched_preempt(recomp_context* ctx) {
    // Always reload first: a syscall-free spin must not call back every
    // iteration, and this self-primes the memset-zero initial budget.
    if (ctx) {
        ctx->preempt_budget = SCHED_PREEMPT_BUDGET;
    }
    if (!sched_preempt_enabled()) {
        return;  // DEFAULT-OFF: dead-effect counter, no yield.
    }
    // Flag ON (not enabled by default in this unit): take a fair cooperative
    // yield. Step 3 swaps this for the real preemptive yield + memory fence.
    sched_yield_point();
}

// ---------------------------------------------------------------------------
// sched_token_release_for_wait / _reacquire_after_wait — HLE object-cv parks
//
// Called by HLE wait stubs that park on their own object condvar (not the
// scheduler WAIT path) around the wait_for. Flag-gated no-ops when OFF. Takes
// g_sched_mutex internally — must NOT be called while it is held. The thread's
// scheduler status is left untouched (the HLE stub manages its own object
// waiter bookkeeping); only the run-token is handed off / reclaimed.
// ---------------------------------------------------------------------------
void sched_token_release_for_wait() {
    if (!token_enabled() || !g_current) {
        return;
    }
    std::unique_lock<std::mutex> lock(g_sched_mutex);
    // Mark ineligible: this thread is about to park on a foreign object cv
    // (sema/eventflag), so the token must NOT be handed back to it until it
    // re-acquires via sched_token_reacquire_after_wait().
    g_current->token_parked = true;
    if (g_token_holder == g_current) {
        g_token_holder = nullptr;
        token_release_to(g_current);
    }
}

void sched_token_reacquire_after_wait() {
    if (!token_enabled() || !g_current) {
        return;
    }
    std::unique_lock<std::mutex> lock(g_sched_mutex);
    token_acquire(lock, g_current);
}

// ---------------------------------------------------------------------------
// sched_token_reacquire_unlocked — [F1] un-nest the blocking token re-acquire.
//
// On entry the caller holds `lock` on its OBJECT/RENDER mutex (taken on cv-wake;
// std::condition_variable re-locks it before wait_for/wait returns). The blocking
// token re-acquire (token_acquire, which parks on self's cv when another thread
// holds the token) MUST NOT run while that object mutex is held, or the parked
// thread strands the object mutex and forms an AB-BA deadlock with the producer
// that needs it (the PREEMPT=1 livelock RCA, .planning/research/
// sched-d-hazard1-rca.md §2.1). So: drop the object mutex, re-acquire the token
// with NO object mutex held, then re-take the object mutex. The caller re-checks
// its wait predicate after this returns (the object state may have changed while
// the mutex was dropped).
//
// OFF (PSPRECOMP_PREEMPT unset): a literal no-op — `lock` is NOT unlocked or
// relocked, so the caller's mutex stays held across exactly the same critical
// section as before this F1 change (OFF byte-identical, zero extra mutex ops).
// ---------------------------------------------------------------------------
void sched_token_reacquire_unlocked(std::unique_lock<std::mutex>& lock) {
    if (!token_enabled() || !g_current) {
        return;  // OFF / non-PSP thread: leave `lock` exactly as received.
    }
    // Drop the object mutex BEFORE the blocking token re-acquire, then re-take
    // it. token_acquire takes g_sched_mutex internally (object_mtx is NOT held
    // here, so the disciplined order object_mtx -> g_sched_mutex is preserved
    // and no object mutex can be stranded across the block).
    lock.unlock();
    {
        std::unique_lock<std::mutex> sched_lock(g_sched_mutex);
        token_acquire(sched_lock, g_current);
    }
    lock.lock();
}

// ---------------------------------------------------------------------------
// psp_thread_exit_current — mark DEAD, decrement counter, wake next
// ---------------------------------------------------------------------------
void psp_thread_exit_current() {
    std::unique_lock<std::mutex> lock(g_sched_mutex);
    if (!g_current) {
        return;
    }

    g_current->status = DEAD;
    g_alive_threads.fetch_sub(1);

    // Token hand-off (flag-gated): an exiting holder must transfer the token to
    // a runnable peer so guest execution does not stall. If this thread is not
    // the holder (OFF, or token elsewhere) this is a no-op. If no peer is
    // runnable the token is left free (token_acquire's safety valve reclaims it).
    if (token_enabled() && g_token_holder == g_current) {
        g_token_holder = nullptr;
        token_release_to(g_current);  // selects+grants a peer (excludes self)
    }

    // Find next READY thread and wake it
    for (int i = 0; i < MAX_THREADS; i++) {
        PspThread& t = g_threads[i];
        if (t.in_use && t.status == READY) {
            t.status = RUNNING;
            t.cv.notify_one();
            break;
        }
    }

    // Notify anyone waiting on this thread (e.g. sceKernelWaitThreadEnd)
    g_current->cv.notify_all();
}

// ---------------------------------------------------------------------------
// psp_scheduler_shutdown — signal exit, wake all, join with timeout
// ---------------------------------------------------------------------------
void psp_scheduler_shutdown() {
    g_should_exit.store(true);

    // Wake all threads so they can observe the exit flag
    {
        std::unique_lock<std::mutex> lock(g_sched_mutex);
        for (int i = 0; i < MAX_THREADS; i++) {
            if (g_threads[i].in_use) {
                g_threads[i].cv.notify_all();
            }
        }
    }

    // Join all threads with 2-second timeout (detach as fallback)
    for (int i = 0; i < MAX_THREADS; i++) {
        PspThread& t = g_threads[i];
        if (t.in_use && t.host_thread.joinable()) {
            // Use a helper thread to implement join-with-timeout
            std::atomic<bool> joined{false};
            std::thread joiner([&] {
                t.host_thread.join();
                joined.store(true);
            });

            // Wait up to 2 seconds
            auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::seconds(2);
            while (!joined.load() &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(10)
                );
            }

            if (joined.load()) {
                joiner.join();
            } else {
                // Thread did not join in time — detach (process is exiting)
                std::fprintf(
                    stderr,
                    "SCHED: thread %d (%s) did not join in 2s, "
                    "detaching\n",
                    t.id, t.name
                );
                joiner.detach();
                t.host_thread.detach();
            }

            t.status = DEAD;
            t.in_use = false;
        }
    }
}

// ---------------------------------------------------------------------------
// psp_get_current_thread — thread-local accessor
// ---------------------------------------------------------------------------
PspThread* psp_get_current_thread() {
    return g_current;
}

// ---------------------------------------------------------------------------
// psp_get_thread — by-id accessor (no mutex, caller ensures DORMANT)
// ---------------------------------------------------------------------------
PspThread* psp_get_thread(int thid) {
    if (thid < 0 || thid >= MAX_THREADS) {
        return nullptr;
    }
    PspThread& t = g_threads[thid];
    if (!t.in_use) {
        return nullptr;
    }
    return &t;
}

// ---------------------------------------------------------------------------
// psp_thread_sleep_current — PPSSPP wakeupCount sleep semantics
// ---------------------------------------------------------------------------
int psp_thread_sleep_current() {
    if (!g_current) {
        return -1;
    }

    std::unique_lock<std::mutex> lock(g_sched_mutex);

    // If wakeup_count > 0, return immediately (pre-woken)
    if (g_current->wakeup_count > 0) {
        g_current->wakeup_count--;
        return 0;
    }

    // Enter WAIT_SLEEP state
    g_current->status = WAIT_SLEEP;
    std::strncpy(g_current->wait_reason, "sleep",
                 sizeof(g_current->wait_reason) - 1);

    PspThread* self = g_current;

    // Token hand-off (flag-gated): a blocking holder transfers the token to a
    // runnable peer BEFORE it parks, so guest execution continues. WAIT_SLEEP
    // is excluded from select_next_runnable, so self cannot re-receive it here.
    if (token_enabled() && g_token_holder == self) {
        g_token_holder = nullptr;
        token_release_to(self);
    }

    // Wake next READY thread before sleeping (prevents deadlock)
    for (int i = 0; i < MAX_THREADS; i++) {
        PspThread& t = g_threads[i];
        if (t.in_use && t.status == READY && &t != g_current) {
            t.status = RUNNING;
            t.cv.notify_one();
            break;
        }
    }

    // Block until woken (5s safety valve)
    self->cv.wait_for(
        lock,
        std::chrono::seconds(5),
        [&] {
            return self->status != WAIT_SLEEP ||
                   g_should_exit.load();
        }
    );

    // Safety valve: if still WAIT_SLEEP after timeout, force-resume
    if (self->status == WAIT_SLEEP) {
        self->status = RUNNING;
        std::fprintf(stderr,
            "SCHED: thread %d (%s) sleep safety valve "
            "triggered\n", self->id, self->name);
    }
    self->wait_reason[0] = '\0';

    // Re-acquire the token before returning to guest code (flag-gated no-op OFF).
    token_acquire(lock, self);

    return 0;
}

// ---------------------------------------------------------------------------
// psp_thread_wakeup — wake sleeping thread or pre-increment counter
// ---------------------------------------------------------------------------
int psp_thread_wakeup(int thid) {
    std::unique_lock<std::mutex> lock(g_sched_mutex);

    if (thid < 0 || thid >= MAX_THREADS) {
        return -1;
    }
    PspThread& t = g_threads[thid];
    if (!t.in_use) {
        return -1;
    }

    if (t.status == WAIT_SLEEP) {
        // Thread is sleeping -- wake it
        t.status = READY;
        t.cv.notify_one();
    } else {
        // Not sleeping -- pre-increment for future SleepThread call
        t.wakeup_count++;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// psp_thread_wait_end — block until target thread reaches DEAD
// ---------------------------------------------------------------------------
int psp_thread_wait_end(int thid, int timeout_us) {
    if (!g_current) {
        return -1;
    }

    std::unique_lock<std::mutex> lock(g_sched_mutex);

    if (thid < 0 || thid >= MAX_THREADS) {
        return SCE_KERNEL_ERROR_NOT_FOUND_THREAD;
    }
    PspThread& t = g_threads[thid];

    // Already dead or freed -- return success immediately
    if (t.status == DEAD || !t.in_use) {
        return 0;
    }

    // Block current thread, wake next READY thread
    PspThread* self = g_current;
    self->status = WAIT;
    std::snprintf(self->wait_reason, sizeof(self->wait_reason),
                  "thread_end:%d", thid);

    // Token hand-off (flag-gated): release to a runnable peer before parking.
    if (token_enabled() && g_token_holder == self) {
        g_token_holder = nullptr;
        token_release_to(self);
    }

    for (int i = 0; i < MAX_THREADS; i++) {
        PspThread& other = g_threads[i];
        if (other.in_use && other.status == READY &&
            &other != self) {
            other.status = RUNNING;
            other.cv.notify_one();
            break;
        }
    }

    // Wait for target thread to die
    auto deadline_dur = (timeout_us > 0)
        ? std::chrono::microseconds(timeout_us)
        : std::chrono::microseconds(5000000);  // 5s safety valve

    bool done = t.cv.wait_for(
        lock,
        deadline_dur,
        [&] {
            return t.status == DEAD || !t.in_use ||
                   g_should_exit.load();
        }
    );

    self->status = RUNNING;
    self->wait_reason[0] = '\0';

    // Re-acquire the token before returning to guest code (flag-gated no-op OFF).
    token_acquire(lock, self);

    if (!done && t.status != DEAD && t.in_use) {
        return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// psp_thread_wait — set thread to WAIT status
// ---------------------------------------------------------------------------
void psp_thread_wait(int thid) {
    std::unique_lock<std::mutex> lock(g_sched_mutex);
    if (thid < 0 || thid >= MAX_THREADS) {
        return;
    }
    PspThread& t = g_threads[thid];
    if (!t.in_use) {
        return;
    }
    if (t.status == RUNNING || t.status == READY) {
        t.status = WAIT;
        if (t.wait_reason[0] == '\0') {
            std::strncpy(t.wait_reason, "wait", sizeof(t.wait_reason) - 1);
        }
    }
}

// ---------------------------------------------------------------------------
// psp_thread_resume — set thread to READY and notify
// ---------------------------------------------------------------------------
void psp_thread_resume(int thid) {
    std::unique_lock<std::mutex> lock(g_sched_mutex);
    if (thid < 0 || thid >= MAX_THREADS) {
        return;
    }
    PspThread& t = g_threads[thid];
    if (!t.in_use) {
        return;
    }
    if (t.status == WAIT) {
        t.status = READY;
        t.wait_reason[0] = '\0';
        t.cv.notify_one();
    }
}

// ---------------------------------------------------------------------------
// [#35] Debug socket support — wait-reason notes + thread snapshot
// ---------------------------------------------------------------------------

void psp_thread_note_wait(const char* reason) {
    if (!g_current || !reason) {
        return;
    }
    std::strncpy(g_current->wait_reason, reason,
                 sizeof(g_current->wait_reason) - 1);
    g_current->wait_reason[sizeof(g_current->wait_reason) - 1] = '\0';
}

void psp_thread_clear_wait() {
    if (!g_current) {
        return;
    }
    g_current->wait_reason[0] = '\0';
}

int psp_scheduler_snapshot(PspDebugThreadInfo* out, int max) {
    static const char* kStatusNames[] = {
        "DORMANT", "READY", "RUNNING", "WAIT", "DEAD", "WAIT_SLEEP"
    };
    std::unique_lock<std::mutex> lock(g_sched_mutex);
    int n = 0;
    for (int i = 0; i < MAX_THREADS && n < max; i++) {
        PspThread& t = g_threads[i];
        if (!t.in_use) {
            continue;
        }
        PspDebugThreadInfo& info = out[n++];
        info.id = t.id;
        std::strncpy(info.name, t.name, sizeof(info.name) - 1);
        info.name[sizeof(info.name) - 1] = '\0';
        int s = static_cast<int>(t.status);
        const char* status = (s >= 0 && s <= 5) ? kStatusNames[s] : "?";
        std::strncpy(info.status, status, sizeof(info.status) - 1);
        info.status[sizeof(info.status) - 1] = '\0';
        std::strncpy(info.wait_reason, t.wait_reason,
                     sizeof(info.wait_reason) - 1);
        info.wait_reason[sizeof(info.wait_reason) - 1] = '\0';
    }
    return n;
}
