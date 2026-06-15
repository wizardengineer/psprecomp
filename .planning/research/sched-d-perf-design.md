# Scheduler approach (d) — single-runnable token: PERF design (implementation-ready, v3)

**Status:** design-before-code, AUTHORITATIVE. Read-only research pass off `feat/single-runnable @
ce95f13`. The token mechanism is IMPLEMENTED, deadlock-free, and default-OFF byte-identical. The
problem is throughput collapse under `PSPRECOMP_PREEMPT=1` (the .hack//Link CRI-ring
producer/consumer scheduling-order race; PSP single-CPU ordering must be restored). This v3
supersedes v2. The changes from v2 are:

1. **DROP M3 (R1).** v2 proposed making the three PARK sites symmetric with `token_handoff`
   (keep-token-when-no-peer). That is WRONG and reintroduces a deadlock. The three park sites
   (`release_for_wait`, `psp_thread_sleep_current`, `psp_thread_wait_end`) KEEP the existing
   ce95f13 `g_token_holder = nullptr` void-release. Symmetric keep-token is correct ONLY in
   `token_handoff`, where the holder stays RUNNING. Full reasoning in §C.R1.
2. **M5/F1 predicate now also accepts a FREE token (R2):** `g_token_holder == self ||
   g_token_holder == nullptr || g_should_exit`. With R1's void-release a peer already parked in the
   `token_acquire` wait loop must be able to claim a freed token; the in-body `nullptr`-claim is
   KEPT and is now serialized by this predicate. §C.R2.
3. **WIRE `sceKernelSendMsgPipe` (R3 — v2 audit miss).** v2 declared MsgPipe "completes
   immediately … nothing to wire." FALSE: the full-pipe path is a bounded busy-poll (~5s,
   `sleep_for(1ms)`) waiting for a CONSUMER PSP thread to drain — a producer-waits-on-consumer
   park that holds the token. WIRE it. §B.M1.d, §A.10 row 16.
4. **Exhaustive runtime-wide re-audit (R4).** The WHOLE `runtime/src` tree was grepped, not just
   the kernel HLE files. Complete site table in §A.10; every real PSP-thread park is wired, every
   render/host site is justified-out.

`token_parked` keeps its meaning: TRUE only while a thread is parked on a FOREIGN object cv (between
`release_for_wait` and `reacquire_after_wait`), NOT while it parks on its own cv inside
`token_acquire`. **New debug-assert invariant (R1): a `token_parked` thread is NEVER
`g_token_holder`.**

This doc is the single authoritative, implementation-ordered spec.
**Order: M1 (audit+wire) → M2 → M4 → M5/F1 → F2 → F3 → F4 → verify.** Earlier steps are
preconditions for later ones; do not reorder. (M3 is intentionally absent — dropped per R1.)

All token ops are gated behind `token_enabled()` == `sched_preempt_enabled()` (reads
`PSPRECOMP_PREEMPT`, cached once, `runtime/src/psp_scheduler.cpp:367-378`). **Every change below
must preserve that gate: OFF == byte-identical no-op.** Lock order `object_mtx → g_sched_mutex` is
acyclic and MUST NOT be reversed (the token helpers take `g_sched_mutex` while the HLE stub already
holds the object mutex).

---

## A. FACTUAL MAP of the token mechanism as it ACTUALLY exists (file:line, verified @ ce95f13)

### A.0 State variables (who reads/writes, under what lock)

| Symbol | Decl | Guard | Meaning |
|---|---|---|---|
| `g_token_holder` (`PspThread*`) | `psp_scheduler.cpp:37` | `g_sched_mutex` | The one thread allowed to run guest code. `nullptr` only at startup, between holders at a park site (R1 void-release), or after the last thread exits. |
| `g_rr_cursor` (`int`) | `psp_scheduler.cpp:45` | `g_sched_mutex` | Within-priority round-robin cursor = slot index last granted. |
| `PspThread::token_parked` (`bool`) | `psp_scheduler.h:78`, default `false` | `g_sched_mutex` | TRUE while this thread released the token to park on a **foreign object cv** — excluded from `select_next_runnable`. **INVARIANT (R1): a `token_parked` thread is never `g_token_holder`.** |
| `recomp_context::preempt_budget` (`int32_t`) | `output/include/recomp.h` | thread-private | Back-edge instruction budget. Decremented inline by emitted code; reloaded by `sched_preempt`. |
| `g_sched_mutex` (`std::mutex`) | `psp_scheduler.cpp:10` | — | Global scheduler lock. Held transiently; never across guest code or an object-cv wait. |
| `g_should_exit` (`std::atomic<bool>`) | `psp_runtime.h` | atomic | Shutdown flag; escape predicate everywhere. |

`token_parked` lifecycle @ ce95f13: set `true` ONLY in `sched_token_release_for_wait` (`:411`); set
`false` in `token_acquire` AFTER the wait loop (`:120`) and on the first-free fast path (`:110`).
The scheduler-managed parks (`psp_thread_sleep_current`, `psp_thread_wait_end`) do NOT set
`token_parked` — they rely on `select_next_runnable`'s `status != READY && != RUNNING` test (`:68`)
to exclude self (WAIT/WAIT_SLEEP status). This stays true in v3 (M2 only moves the clear earlier).

### A.1 `select_next_runnable(exclude)` — `psp_scheduler.cpp:50-76`
Picks the grantee. Caller holds `g_sched_mutex`. Scans 64 slots from `(g_rr_cursor+1+n)%64`
(`:54`); skips `!in_use`/`==exclude` (`:56`), `token_parked` (`:65`), `status != READY && != RUNNING`
(`:68`); keeps the lowest `priority` number (`:71`), ties by scan order (RR). Returns `nullptr` if
none. Does NOT move `g_rr_cursor` — `token_release_to` does.

**The soundness predicate of the whole scheme** (`:59-64` comment): a `token_parked` thread is
excluded because it "released the token and is blocked on a foreign object cv … granting the token
to them would lose it." That comment is TRUE only if every foreign-object-cv park sets
`token_parked`. M1 establishes that for every real PSP-thread park; until M1, it is false for
mutex/lwmutex/WaitSema-deleted and SendMsgPipe.

### A.2 `token_release_to(self)` — `psp_scheduler.cpp:82-98`
Holds `g_sched_mutex`; no-op if `!token_enabled()`. `next = select_next_runnable(self)`; if
`nullptr` **return keeping the token** (`:88`); else `g_token_holder = next`, set `g_rr_cursor` to
next's slot (`:91-96`), `next->cv.notify_one()` (`:97`). A directed hand-off.

**Critical R1 distinction:** the THREE park sites do NOT call `token_release_to` on a holder that
keeps the token. They first set `g_token_holder = nullptr` (void-release), THEN call
`token_release_to(self)` — so when no peer is runnable the token is left `nullptr` (free), NOT kept
on the parking thread. This is INTENTIONAL and load-bearing (see §C.R1). `token_handoff` is the only
caller that relies on `token_release_to`'s keep-token-when-no-peer return.

### A.3 `token_acquire(lock, self)` — `psp_scheduler.cpp:103-121`
Holds `g_sched_mutex`; no-op if `!token_enabled()`. First-free fast path: if `g_token_holder==nullptr`
take it, `token_parked=false`, return (`:108-112`). Else bare loop while `g_token_holder!=self &&
!g_should_exit` (`:113`): `self->cv.wait_for(lock, 50ms)` (`:114`, `SCHED_TIMEOUT_MS=50`
`psp_scheduler.h:47`) with NO predicate lambda, plus a `nullptr`-claim valve inside the loop
(`:116-118`). On loop exit: `token_parked=false` (`:120`). The bare `wait_for` recovers a
missed/raced notify only after a full 50ms → the perf collapse. M5/F1 fixes this; M2 moves the
`token_parked=false` to the top.

### A.4 `token_handoff(lock, self)` — `psp_scheduler.cpp:126-135`
No-op if `!token_enabled() || g_token_holder != self`. Body: `token_release_to(self)` (`:130`); if
still holder return (`:131-133` — KEPT the token, no peer); else `token_acquire` (`:134`). This is
the ONLY symmetric keep-token site (the holder stays RUNNING throughout — it never blocks, it just
yields). F3 short-circuits it when self is the sole runnable.

### A.5 `sched_yield_point()` — `psp_scheduler.cpp:288-353`
ON path (`:302-305`): `token_handoff(lock, g_current); return;`. OFF path (`:307-353`): unchanged
parallel scan + `wait_for(50ms)` — **must stay byte-identical.**

### A.6 `sched_preempt(ctx)` — back-edge hook, `psp_scheduler.cpp:380-392`
Emitted inline `if (--ctx->preempt_budget <= 0) sched_preempt(ctx);` at loop back-edges
(`crates/psp-emitter/src/function.rs:706`; e.g. `output/generated/batch_0000.cpp`). Always reloads
`ctx->preempt_budget = SCHED_PREEMPT_BUDGET` (=100000, `psp_scheduler.h:57`) (`:383-385`); if
`!sched_preempt_enabled()` return (`:386-388`); ON → `sched_yield_point()` (`:391`) → A.5 ON → A.4.

### A.7 `sched_token_release_for_wait` / `_reacquire_after_wait` — `psp_scheduler.cpp:403-424`
HLE object-cv park helpers; each takes `g_sched_mutex` internally (lock order `object_mtx →
g_sched_mutex`). No-op if `!token_enabled() || !g_current`.
`release_for_wait` (`:403-416`): `g_current->token_parked = true` (`:411`); if
`g_token_holder==g_current`: `g_token_holder = nullptr; token_release_to(g_current)` (`:412-415`).
**This is the void-release R1 KEEPS.** It nulls the holder FIRST, then `token_release_to` grants to a
peer if one exists; with no peer the token is left `nullptr` (free) for a re-contender to claim. This
is correct because `self` is about to BLOCK on a foreign cv — keeping the token on it would strand
the token on a non-running thread (see §C.R1).
`reacquire_after_wait` (`:418-424`): `token_acquire(lock, g_current)`.

### A.8 The scheduler-managed parks
- `psp_thread_sleep_current` (`:542-603`): WAIT_SLEEP; if holder `g_token_holder=nullptr;
  token_release_to(self)` (`:565-568`) — same void-release; self excluded by WAIT_SLEEP status (not
  `token_parked`); park on own cv 5s valve (`:581`); on wake `token_acquire` (`:600`). **R1: KEEP
  the void-release.**
- `psp_thread_wait_end` (`:634-698`): WAIT; void-release `token_release_to` at `:658-661`;
  `token_acquire` at `:691`. **R1: KEEP the void-release.**
- `psp_thread_exit_current` (`:429-459`): dying holder nulls + `token_release_to` (`:442-445`) —
  correct to leave free (self is dying, will never reacquire). KEEP.

### A.9 Object-cv signal sources (who issues the notify that wakes each park)
| Park cv | Woken by | At |
|---|---|---|
| `ef->cv` | SetEventFlag / DeleteEventFlag `notify_all` | eventflag.cpp signal/delete |
| `s->cv` | SignalSema `notify_all` (`sema_grant_waiters` → `:223`) | sema.cpp |
| `m->cv` (mutex) | UnlockMutex `notify_one` | mutex.cpp:110 |
| `m->cv` (lwmutex) | UnlockLwMutex / DeleteLwMutex `notify_*` | lwmutex.cpp:273 + unlock |
| MsgPipe FIFO | TryReceiveMsgPipe drains (no cv today — R3 adds one or polls) | utility.cpp:1767-1769 |
| `self->cv` (token) | `token_release_to`/grant `notify_one` | scheduler |

### A.10 RUNTIME-WIDE PARK INVENTORY — EXHAUSTIVE (R4; whole `runtime/src` grepped)

Greps run over ALL of `runtime/src` (not just `hle/`): `cv.wait|wait_for|wait_until`,
`this_thread::sleep_for|yield`, `for(;;)|while(true)|spin|poll|retry`, and every
`MsgPipe|Fpl|Vpl|Mbx|Alarm|VTimer` symbol. Every site that can hold the token while waiting on
another PSP thread, plus every host/render wait, classified:

| # | Site (file:line) | Kind | Blocks on a PSP peer? | Wired @ ce95f13? | v3 action |
|---|---|---|---|---|---|
| 1 | `psp_hle_kernel_eventflag.cpp:194` | `ef->cv.wait_for` 5s | YES | YES (`:193`/`:197`) | keep; F2 gates leading yield `:164` |
| 2 | `psp_hle_kernel_sema.cpp:269` | `s->cv.wait_for` 100ms (deleted path) | YES | **NO** | **WIRE (M1.a)** release before/reacquire after |
| 3 | `psp_hle_kernel_sema.cpp:329` | `s->cv.wait_for` timed (slow) | YES | YES (`:324`/`:338`/`:364`) | keep |
| 4 | `psp_hle_kernel_sema.cpp:346` | `s->cv.wait_for` 5s (slow unbounded) | YES | YES (`:324`/`:357`/`:364`) | keep |
| 5 | `psp_hle_kernel_sema.cpp:456` | `s->cv.wait_for` 5ms (CB poll) | YES | YES (`:455`/`:460`) | keep; M4 gates 5→100ms |
| 6 | `psp_hle_kernel_mutex.cpp:73` | `m->cv.wait` **UNBOUNDED, no exit** | YES | **NO** | **WIRE + add finite valve (M1.b)** |
| 7 | `psp_hle_kernel_lwmutex.cpp:186` | `m->cv.wait_until` deadline | YES | **NO** | **WIRE (M1.c)** bracket whole for-loop |
| 8 | `psp_hle_kernel_lwmutex.cpp:188` | `m->cv.wait_for` 5s valve | YES | **NO** | **WIRE (M1.c)** same bracket as #7 |
| 9 | `psp_hle_utility.cpp:1695-1727` | `SendMsgPipe` busy-poll ~5s, `sleep_for(1ms)` | **YES (consumer drains)** | **NO** | **WIRE (M1.d, R3)** release around the full-pipe wait |
| 10 | `psp_hle_utility.cpp:1734-1775` | `TryReceiveMsgPipe` | NO (single lock, returns) | n/a | **leave out** — genuinely non-blocking |
| 11 | `psp_hle_kernel_thread.cpp:307` | `DelayThread` `sleep_for(usec)` | NO (real-time host sleep) | n/a | **leave out** — §E |
| 12 | `psp_hle_utility.cpp:188` | `audio_block_for_samples` `sleep_for` | NO (real-time pacing) | n/a | **leave out** — §E |
| 13 | `psp_hle_display.cpp:57` | vblank `sleep_for(16667us)` | NO (real-time 60Hz) | n/a | **leave out** — §E |
| 14 | `psp_hle_io.cpp` (all `sched_yield_point`) | leading yields only; no cv/sleep/spin park | NO | n/a | **leave out** — no park; F2 does not touch (not object-cv parks) |
| 15 | `psp_scheduler.cpp:114` | `self->cv` token_acquire | self-cv, IS the token wait | n/a | the wait itself (M5/F1) |
| 16 | `psp_scheduler.cpp:226` | `t->cv` thread_entry gate | startup gate, not a guest park | n/a | leave |
| 17 | `psp_scheduler.cpp:339` | `current->cv` OFF yield path | OFF-only | n/a | leave (OFF byte-identical) |
| 18 | `psp_scheduler.cpp:581` | `self->cv` sleep_current | scheduler-managed (status-excluded) | n/a | R1 void-release kept |
| 19 | `psp_scheduler.cpp:678` | `t->cv` wait_end | scheduler-managed (status-excluded) | n/a | R1 void-release kept |
| 20 | `psp_scheduler.cpp:493` | `sleep_for(10ms)` shutdown join | host-only, shutdown | n/a | leave |
| 21 | `psp_render_queue.cpp:54` | `g_render_cv.wait` (present/SetFrameBuf) | **YES — called on a PSP thread** | **NO** | **WIRE (M1.e)** bracket the cv.wait — final-audit must-fix |
| 22 | `psp_render_queue.cpp:127` | `g_ge_done_cv.wait_for` (DrawSync/ListSync) | **YES — called on a PSP thread** | **NO** | **WIRE (M1.e)** bracket the cv.wait_for — final-audit must-fix |
| 23 | `psp_ge_draw.cpp:873` | `g_ss_req_cv.wait_for` | GE/screenshot req, NOT a PSP thread | n/a | **leave out** — §E |

**Sites needing NEW wiring (M1): #2, #6, #7+#8, #9, #21, #22.** Sites #1, #3-#5 already wired.
(#21/#22 — the render-queue present/DrawSync parks — were re-classified in the FINAL audit:
they are reached on PSP guest threads (`g_current != null`), so they ARE peer parks and must be
wired — see M1.e. The MAIN-thread `render_queue_process` GE finish/signal callback is a separate
DEFERRED concern, §E.) **NOT present in
the runtime at all (grep returned zero registered handlers):** `sceKernelReceiveMsgPipe` (blocking
receive — only the non-blocking `TryReceiveMsgPipe` exists), and every FPL/VPL fixed/variable pool,
message box (Mbx), alarm, and vtimer blocking primitive. If any of these are added later they MUST
follow the M1 wiring pattern; that is a forward note, not a current site.

**Eventflag/lwmutex/mutex/sema/msgpipe — every blocking entry's *CB variant* delegates to its base
function** (`WaitEventFlagCB:229→WaitEventFlag`, `LockMutexCB:87→LockMutex`,
`LockLwMutexCB:304→LockLwMutex`, `WaitSemaCB` has its own poll loop #5; `DelayThreadCB`,
`SleepThreadCB`, `WaitThreadEndCB`, `DelayThreadCB` likewise delegate). Wiring the base function
covers the CB variant automatically.

**INVARIANT (state as a comment above `select_next_runnable` `:50`):** *"`select_next_runnable`
excludes every `token_parked` thread; this is sound ONLY because every object-cv / busy-poll park
that can block a PSP thread (eventflag, sema slow/CB/deleted, mutex, lwmutex, SendMsgPipe-full)
brackets its wait with `sched_token_release_for_wait()`/`sched_token_reacquire_after_wait()`,
holding `token_parked` for the entire park. A `token_parked` thread is NEVER `g_token_holder` (the
three park sites void-release the token before parking). Render-thread cvs and real-time host sleeps
(DelayThread, audio, vblank) are intentionally NOT bracketed — they are not PSP-thread peer parks."*

---

## B. IMPLEMENTATION-ORDERED SPEC

Design principle for ALL steps: OFF gate unchanged; every new branch reached only when
`token_enabled()`/`sched_token_enabled()`. No emitter change (back-edge points already emitted in
both outputs). No new lock; no reversal of `object_mtx → g_sched_mutex`.

### M1 (PRECONDITION — do FIRST) — Wire the four missing PSP-thread parks

Without M1, the `token_parked` exclusion (A.1) and F3's sole-runnable fast path are UNSOUND: a
holder that parks on the mutex/lwmutex/WaitSema-deleted cv or the SendMsgPipe busy-poll without
setting `token_parked` stays visible to `select_next_runnable` as READY/RUNNING, so a peer could
"grant" it the token while it is asleep on a foreign cv → lost token → stall. M1 closes this for
every site. Each wired park sets `token_parked=true` and void-releases (R1) before parking, then
`reacquire_after_wait` after.

**M1.a — WaitSema deleted path (`psp_hle_kernel_sema.cpp:267-278`).** Bracket the 100ms park:
```cpp
std::unique_lock<std::mutex> lock(s->mtx);
s->wait_count++;
sched_token_release_for_wait();                       // M1.a: was unwired
bool got_it = s->cv.wait_for(lock, std::chrono::milliseconds(100),
    [&] { return s->current_count >= signal && s->waiters.empty(); });
sched_token_reacquire_after_wait();                   // M1.a: was unwired
if (got_it) { s->current_count -= signal; }
s->wait_count--;
```
(No leading-yield change here — the leading `sched_yield_point()` at `:236` is shared and handled by
F2.)

**M1.b — LockMutex (`psp_hle_kernel_mutex.cpp:68-79`) — wire AND add a finite valve.** Today it is
`m->cv.wait(lock, pred)` — UNBOUNDED, no `g_should_exit` escape, no `token_parked`. Replace with a
finite-valve loop bracketed by the token helpers:
```cpp
std::unique_lock<std::mutex> lock(m->mtx);
PspThread* t = psp_get_current_thread();
int my_thid = t ? t->id : 0;
auto acquired = [&]{ return m->owner_thid == -1 || m->owner_thid == my_thid; };
sched_token_release_for_wait();                       // M1.b: was unwired
while (!acquired() && !g_should_exit.load()) {        // M1.b: finite-valve loop
    m->cv.wait_for(lock, std::chrono::seconds(5),
        [&]{ return acquired() || g_should_exit.load(); });
}
sched_token_reacquire_after_wait();                   // M1.b: was unwired
m->owner_thid = my_thid;
m->lock_count += count;
ctx->r[2] = SCE_OK;
```
The valve only ADDS a wake; correctness on a real signal is unchanged (UnlockMutex still
`notify_one`s at `:110`). This removes the only unbounded park in the HLE.

**M1.c — LockLwMutex (`lw_lock_blocking`, `psp_hle_kernel_lwmutex.cpp:117-192`).** The park is the
`for(;;)` loop at `:147`, whose body waits at `:186` (`wait_until` deadline) or `:188` (`wait_for` 5s
valve). Bracket the WHOLE loop — the thread is parked for the loop's lifetime:
```cpp
// after psp_thread_note_wait(tag) at :129, before the for(;;) at :147:
sched_token_release_for_wait();                       // M1.c: was unwired
for (;;) {
    ... existing body (lw_fast_lock retry, deleted check, timeout, cv waits :186/:188) ...
}
psp_thread_clear_wait();                              // existing :191
sched_token_reacquire_after_wait();                   // M1.c: covers every break
```
Place `release_for_wait` once before the loop and `reacquire_after_wait` once after it (every `break`
falls through to it). All branches inside hold `g_lw_mtx` only, never `g_sched_mutex`, so lock order
is preserved (the helpers take `g_sched_mutex` internally — called with only the object mutex held,
which is correct).

**M1.d — SendMsgPipe full-pipe wait (R3, `psp_hle_utility.cpp:1683-1728`).** The busy-poll at
`:1695-1727` waits up to ~5s (`sleep_for(1ms)` per spin) for a CONSUMER PSP thread to `TryReceive`
and drain the FIFO. Under ON it holds the token across the whole spin → the consumer can never run →
permanent full-pipe stall recovered only by the 5000-spin cap (corrupting the byte stream). Two
acceptable forms (flag-gated, OFF byte-identical either way):

*Preferred (low-risk cv conversion):* add a `std::condition_variable not_full` and a counter to
`PspMsgPipe`; `TryReceiveMsgPipe` calls `mp.not_full.notify_all()` after it pops; `SendMsgPipe`
waits on it. Bracket the wait:
```cpp
// inside SendMsgPipe, replacing the sleep_for(1ms) backstop with a cv wait:
if (mp.fifo.size() + size <= mp.buf_size) { ...push, return SCE_OK... }
// pipe full:
if (g_should_exit.load() || spins >= 5000) { ctx->r[2] = SCE_KERNEL_ERROR_MPP_FULL; return; }
sched_token_release_for_wait();                       // M1.d: token to the consumer
mp.not_full.wait_for(lock, std::chrono::milliseconds(1),
    [&]{ return mp.fifo.size() + size <= mp.buf_size || g_should_exit.load(); });
sched_token_reacquire_after_wait();
```
(Note: the existing code releases `g_msgpipe_mtx` between iterations via an inner scope; the cv must
share the same `lock`/`g_msgpipe_mtx` as `TryReceive` for the notify to be correct — restructure so
the lock is held across the wait, matching the eventflag pattern.)

*Acceptable fallback (release/reacquire around the existing `sleep_for`):* keep the busy-poll but
bracket the per-iteration sleep so the consumer can run while the producer is parked:
```cpp
// at :1721-1726, after the full-pipe check, before sleep_for:
sched_token_release_for_wait();                       // M1.d
std::this_thread::sleep_for(std::chrono::milliseconds(1));
sched_token_reacquire_after_wait();                   // M1.d
```
Either form sets `token_parked` for the duration, making SendMsgPipe a sound park under the A.10
invariant. ALSO gate the leading `sched_yield_point()` at `:1693` per F2 (it precedes a potential
park). **`TryReceiveMsgPipe` is left unwired (A.10 #10): it is genuinely non-blocking** (single lock,
returns MPP_EMPTY when short, never loops/sleeps) — there is no blocking `sceKernelReceiveMsgPipe` in
the runtime to wire.

**M1.e — render-queue PSP-thread parks (`psp_render_queue.cpp:54` and `:127`) — FINAL-AUDIT MUST-FIX.**
Both `render_queue_post` (the FramePresent/SetFrameBuf path — note it has NO leading yield) and
`render_queue_draw_sync` (the `sceGeDrawSync`/`ListSync` path) are invoked on PSP guest threads
(`g_current != null`), where they block on the GE/GL round-trip. Under ON, a PSP thread that blocks
there while holding the token starves every peer of the token for the whole GL frame, and can wedge
a token-mediated GE deadlock (the consumer that would drain/finish the list cannot get the token).
Bracket each cv wait with the token helpers:
```cpp
// render_queue_post (:54), around g_render_cv.wait:
sched_token_release_for_wait();
g_render_cv.wait(lock, [] { return g_render_req.done || g_should_exit.load(); });
sched_token_reacquire_after_wait();

// render_queue_draw_sync (:127), around g_ge_done_cv.wait_for (leave the leading :119 yield as-is):
sched_token_release_for_wait();
bool drained = g_ge_done_cv.wait_for(lock, std::chrono::milliseconds(500),
    [] { return g_ge_queue.empty() || g_should_exit.load(); });
sched_token_reacquire_after_wait();
```
These run on PSP threads, so the wiring is effective (the helpers no-op on the main/render thread
where `g_current == null`, and OFF). **DEFERRED:** the main-thread GE finish/signal callback dispatched
inside `render_queue_process` (`g_current == null` there) may itself block; that is NOT fixed by the
PSP-thread wiring above and is a separate concern (one-line code comment added at
`render_queue_process`). The `render_queue_draw_sync` leading `sched_yield_point()` at `:119` is left
in place (it is not an object-cv-park-gate site; it lets the render thread make progress before this
thread blocks) — NOT gated by F2.

**M1 — all flag-gated:** every `sched_token_*` is a no-op when OFF, so OFF is byte-identical for
M1.a/.c/.d/.e. The added LockMutex finite valve (M1.b) changes OFF behavior ONLY in the pathological
missed-notify/shutdown case (previously a permanent hang); the normal `notify_one` path is unchanged
ms-for-ms. The SendMsgPipe cv form (M1.d preferred) changes the full-pipe wake from a 1ms poll to a
notify+1ms-backstop — OFF still parks only when the pipe is full (Patapon's 84-byte/1024-byte pipe
never fills), so the OFF byte-gate (geometry/render verdicts) is unaffected. **State the LockMutex
valve and the MsgPipe cv as the OFF-observable changes in the PR and confirm against the OFF gate.**

**M1 invariant comment:** add the INVARIANT paragraph (end of §A.10) as a comment above
`select_next_runnable` (`:50`).

### M2 — Clear `token_parked` at the TOP of `token_acquire`, before the wait loop

Move `self->token_parked = false;` from after the loop (`:120`) to the very top of `token_acquire`,
before the first-free check (`:107`). A thread re-contending for the token (via
`reacquire_after_wait`, or the sleep/wait-end tails) must be **immediately eligible** so that
`select_next_runnable`/`other_runnable_exists` can grant+notify it directly. Final body folds in
M5/F1 (see below). Skeleton:
```cpp
static void token_acquire(std::unique_lock<std::mutex>& lock, PspThread* self) {
    if (!token_enabled()) return;
    self->token_parked = false;                       // M2: clear FIRST — now a valid grantee
    if (g_token_holder == self)    return;            // already hold it (rare; harmless)
    if (g_token_holder == nullptr) { g_token_holder = self; return; }
    // ... M5/F1 predicated wait loop ...
}
```
**Why this does NOT re-admit the bug `token_parked` prevents:** a thread is `token_parked` only while
*actually parked on the FOREIGN object cv* (between `release_for_wait` and `reacquire_after_wait`).
The instant it returns from the foreign `cv.wait*` and calls `reacquire_after_wait` → `token_acquire`,
it is no longer on the foreign cv; it is now waiting on its OWN cv inside `token_acquire`. A thread
waiting on its own cv is exactly a valid grantee — that is precisely the state in which a
grant+`notify_one` wakes it (A.1's comment: "A thread parked in `token_acquire` (`token_parked` stays
false there) IS eligible"). M2 makes that comment TRUE on every entry, not just after the loop.
Without M2, the window between `reacquire_after_wait` entry and the old `:120` clear left the
re-contending thread invisible, so the .hack producer's wakeup fell to the 50ms valve — the perf
collapse merely relocates from the grant to the reacquire.

The new `if (g_token_holder == self) return;` is a cheap correctness guard (a thread that somehow
still holds the token has nothing to do); it is not the M3 keep-token path (M3 is dropped).

### M4 — Gate the WaitSemaCB poll timeout; introduce `sched_token_enabled()`

Introduce a public accessor `bool sched_token_enabled();` in `psp_scheduler.h`, defined in
`psp_scheduler.cpp` as a one-line forward to the cached flag (`return token_enabled();`). It is the
single public predicate used by M4 and F2. `token_enabled()` stays file-static; `sched_token_enabled`
is its public face — one source of truth.

Then gate the WaitSemaCB poll cadence (`psp_hle_kernel_sema.cpp:456`):
```cpp
s->cv.wait_for(lock,
    std::chrono::milliseconds(sched_token_enabled() ? 100 : 5),   // M4: OFF keeps EXACT 5ms
    [&]{ return (s->current_count >= signal && s->waiters.empty()) || g_should_exit.load(); });
```
OFF retains the exact 5ms callback-pump cadence (byte-identical — Patapon's asset/IO callback pump
must not change OFF); only ON gets the 100ms valve (the cv already wakes on `SignalSema`'s
`notify_all` at `:223`, so 100ms is a pure backstop, not the wake mechanism). This is the gated form
of F4's WaitSemaCB retune — the constant change and the gate are the SAME edit.

### M5 / F1 — Predicated `token_acquire` wait (no missed wake; FREE-token accept per R2)

With R1's void-release the hot release paths CAN leave `g_token_holder == nullptr`, so the predicate
must accept a free token AND the in-body `nullptr`-claim is KEPT. Final `token_acquire` body
(M2 + M5/F1 + R2):
```cpp
static void token_acquire(std::unique_lock<std::mutex>& lock, PspThread* self) {
    if (!token_enabled()) return;
    self->token_parked = false;                       // M2
    if (g_token_holder == self)    return;            // M2 guard
    if (g_token_holder == nullptr) { g_token_holder = self; return; }   // first-free fast path
    while (g_token_holder != self && !g_should_exit.load()) {
        self->cv.wait_for(lock, std::chrono::milliseconds(SCHED_TIMEOUT_MS),
            [&]{ return g_token_holder == self            // granted to me
                     || g_token_holder == nullptr         // R2: freed (void-release) — claimable
                     || g_should_exit.load(); });
        if (g_token_holder == nullptr) {                 // R2: claim the freed token
            g_token_holder = self;
            ++g_valve_nullptr_claims;
        }
    }
}
```
**Lost-wake closed:** the setter writes `g_token_holder = grantee` (or `nullptr` at a void-release)
under `g_sched_mutex` BEFORE `notify_*`; the waiter evaluates the predicate under the SAME
`g_sched_mutex` before parking. A notify (or a void-release) delivered while the waiter is between
predicate-true and park cannot be lost — predicate-true returns from `wait_for` immediately.

**R2 — why this cannot duplicate nor strand the token (the critical proof):**
- *No duplication.* The predicate becoming TRUE on `g_token_holder == nullptr` only WAKES the
  waiter; the actual claim `g_token_holder = self` happens inside the loop body under `g_sched_mutex`
  AFTER `wait_for` returns. Two peers both woken by the same void-release each re-take `g_sched_mutex`
  serially: the first to run sees `nullptr`, sets `g_token_holder = self`, exits the loop (the
  `while` condition `g_token_holder != self` is now false for it). The second re-acquires
  `g_sched_mutex`, re-evaluates the `while` condition: `g_token_holder` is now the first thread
  (≠ self) and ≠ `nullptr`, so it re-parks. Exactly one claimant. The `nullptr`-claim and the
  `while` re-check are both inside the same `g_sched_mutex` critical section → no TOCTOU.
- *No strand.* After a void-release leaves `g_token_holder == nullptr`, every peer currently parked
  in this loop is woken by the void-release's `notify` path (the park site calls
  `token_release_to(self)` after nulling; if a peer is runnable it is granted directly — the common
  case after M2 — else the token stays `nullptr` and ANY parked peer's predicate is now TRUE and it
  claims on its next 50ms tick at worst, but in practice immediately because the releaser's
  `notify_one`/the peer's own re-contention notify fires). The token is therefore never left
  `nullptr` with all peers asleep indefinitely: either a runnable peer was granted directly, or a
  parked peer's FREE-accept predicate fires. The 50ms is a pure backstop.
- *Common case is 0ms (M2 + grant):* a void-release immediately followed by `token_release_to`
  selecting a peer that M2 made eligible grants directly (`g_token_holder = peer; notify_one`) — the
  peer's predicate `g_token_holder == self` is TRUE, it wakes and returns with zero re-parks. The
  FREE-accept branch is the backstop for the already-parked peer that the direct grant did not target
  (e.g. two peers contend, one is granted, the other must claim a later free).

**Instrumentation (ON-only, must not affect OFF):** two file-static `std::atomic<uint64_t>` counters,
`g_valve_timeout_wakes` (incremented when `wait_for` returns `false` = timed out without predicate)
and `g_valve_nullptr_claims` (the FREE-accept claim branch). Increment ONLY inside `token_acquire`,
which is gated `!token_enabled() → return` at the top, so OFF never touches them. Expose via the debug
socket or a periodic stderr line. **Acceptance signal: steady-state ~0 for `timeout_wakes`;
`nullptr_claims` small-and-bounded (it WILL fire on R1 void-releases with multiple parked peers — that
is expected, not a bug; a high/growing rate signals a missed direct-grant).** Counters are plain
increments under `g_sched_mutex` (no perf impact) and OFF-invisible.

### F2 — Drop the leading yield-then-park redundancy in HLE waits

An HLE wait that is going to park on an object cv should release the token ONCE (`release_for_wait`)
and NOT also run the leading `sched_yield_point()` full handoff. Gate every leading
`sched_yield_point()` that precedes an object-cv / busy-poll park:
```cpp
if (!sched_token_enabled()) sched_yield_point();   // OFF: unchanged; ON: skip — release_for_wait does the hand-off
```
Sites: `psp_hle_kernel_eventflag.cpp:164` (WaitEventFlag), `psp_hle_kernel_sema.cpp:236` (WaitSema),
`psp_hle_kernel_mutex.cpp:58` (LockMutex — now that M1.b wired it), `psp_hle_kernel_lwmutex.cpp:283`
(LockLwMutex — now that M1.c wired it), `psp_hle_utility.cpp:1693` (SendMsgPipe — now that M1.d wired
it). OFF runs the original yield unchanged → byte-identical. ON skips it; the subsequent
`release_for_wait` is the sole hand-off.

**Coupling note (fast paths that don't park):** under ON the WaitSema/LockMutex/LockLwMutex/SendMsgPipe
*fast paths* (non-blocking acquire, uid-not-found, garbage-uid, pipe-not-full) now no longer yield.
This is correct for the one-runnable model — a non-blocking acquire is doing guest-visible work, not
blocking, so it should keep the token (F3 makes "keep the token when sole-runnable" the norm). Flag in
residual concerns for the verifier to confirm no guest path relied on a fast-path acquire forcing a
turn rotation. The leading `sched_yield_point()` in `kernel_thread.cpp` (SleepThread `:321`,
WaitThreadEnd `:516`, DelayThread `:306`) is NOT gated here — Sleep/WaitThreadEnd go through the A.8
scheduler-managed release (R1 void-release), not `release_for_wait`; DelayThread is a real-time sleep,
not an object park (§E). Leaving their leading yield ON is harmless (it is a `token_handoff`, which F3
fast-paths to a no-op when sole-runnable).

### F3 — Sole-runnable fast path in `token_handoff`

After the gate in `token_handoff`, if `self` is the only runnable thread, return — skip the
release/reacquire round trip:
```cpp
static bool other_runnable_exists(PspThread* self) {   // define near select_next_runnable
    return select_next_runnable(self) != nullptr;       // reuses the EXACT eligibility predicate
}
static void token_handoff(std::unique_lock<std::mutex>& lock, PspThread* self) {
    if (!token_enabled() || g_token_holder != self) return;
    if (!other_runnable_exists(self)) return;          // F3: sole runnable → keep token, no round trip
    token_release_to(self);
    if (g_token_holder == self) return;                // (redundant given F3 guard, harmless)
    token_acquire(lock, self);
}
```
`other_runnable_exists` == `select_next_runnable(self) != nullptr`, which **already excludes self and
every `token_parked` thread and requires READY/RUNNING** — exactly "is there a valid grantee?". This
is SAFE because **M1** guarantees every object-cv/busy-poll-parked peer is `token_parked` (invisible
to the scan, so a parked producer never counts as "another runnable" and never falsely blocks the
fast path), and **M2** makes a re-contending peer visible the instant it re-enters `token_acquire`
(so a producer that just un-parked IS seen on the consumer's next back-edge). The back-edge hook
`sched_preempt → sched_yield_point → token_handoff` does ZERO cv ops and one 64-slot scan when self
is sole-runnable; the budget was already reloaded in `sched_preempt` (A.6 `:384`) so the spin does
not re-enter every iteration.

**F3 is in `token_handoff` ONLY** (the keep-token-when-RUNNING site). It does NOT apply to the three
R1 park sites — those always void-release because self is about to BLOCK, not keep running.

### F4 — Re-tune the constants (one per measured run)

| Constant | Current | Where | New | Reason |
|---|---|---|---|---|
| `SCHED_PREEMPT_BUDGET` | 100000 | `psp_scheduler.h:57` | **20000** | After F3 a hand-off is ~free when sole-runnable, so the budget's only remaining job is to bound producer-wakeup latency when a peer un-parks (§D). Finer granularity (5× sooner notice) at negligible cost (one scan per 20k back-edges). |
| `token_acquire` 50ms (`SCHED_TIMEOUT_MS`) | 50 | `psp_scheduler.h:47` | **keep 50, demote to backstop** | M5/F1 makes the wake notify/free-accept-driven; 50ms now fires ~never. Do NOT shrink — a short valve under contention busy-re-polls `g_sched_mutex`. |
| WaitSemaCB poll | 5ms | `psp_hle_kernel_sema.cpp:456` | **100ms, GATED (M4)** | cv woken by `SignalSema` `notify_all`; 100ms is a backstop. OFF stays 5ms per M4. |

**Tuning method (staged, one constant per run):** land M1+M2+M4+M5/F1+F2+F3 with
`SCHED_PREEMPT_BUDGET=20000` and WaitSemaCB=100ms-ON first; run the .hack N≥30 freeze-rate harness +
Patapon CLEANROOM + visible gates ON. If .hack progresses but slowly, bisect `SCHED_PREEMPT_BUDGET`
down (10k/5k); if Patapon perf regresses, bisect up. Change ONE constant per measured run (they
interact). Acceptance: OFF byte-identical (HARD), .hack freeze-rate → ~0 PROGRESSING (ge.frames past
~52, heartbeats firing), Patapon ON in-band + visible + deadlock-free, purity green.

---

## C. SELF-REVIEW (per resolution + per step: deadlock / lost-wake / OFF byte-identical / one-runnable)

### C.R1 — Why the THREE park sites KEEP the void-release (DROP M3) — the load-bearing argument

The candidate "symmetric keep-token" (v2's M3) would, at a park site with no runnable peer, leave
`g_token_holder = self` instead of `nullptr`. **This deadlocks.** Trace:

1. Holder H is the sole RUNNING thread; it reaches a park site (e.g. WaitSema slow, full pipe,
   SleepThread) because it is waiting for a SIGNAL that only PEER P can produce — but P is currently
   `token_parked`/READY-but-not-holder or not yet runnable.
2. Under keep-token: H sets `token_parked=true` (or WAIT status), keeps `g_token_holder = H`, and
   blocks on the foreign cv. Now **H is `g_token_holder` AND `token_parked`** — violating the R1
   invariant.
3. P needs the token to run guest code and produce the signal. P calls `token_acquire`. The predicate
   is `g_token_holder == P || g_token_holder == nullptr || exit`. `g_token_holder == H` (not P, not
   nullptr), so P's predicate is FALSE — **P parks forever.** The only `nullptr`-claim valve fires
   solely when the holder is `nullptr`, but the holder is H, not `nullptr`. So no thread can ever take
   the token: H is blocked waiting for P's signal, P is blocked waiting for the token H holds.
4. Recovery only via the 5s object-cv valve on H, which wakes H spuriously and corrupts the CRI ring
   (the exact bug we are fixing).

The void-release (`g_token_holder = nullptr` THEN `token_release_to`) avoids this: with no runnable
peer the token is left `nullptr`, P's `token_acquire` predicate `g_token_holder == nullptr` is TRUE,
P claims it (R2), runs guest code, produces the signal, and wakes H. **A blocking holder MUST release
the token into the free state so a not-yet-runnable peer can grab it.** Keeping the token is only safe
when the holder STAYS RUNNING — i.e. `token_handoff` (yield), where H does not block on any peer
signal and will itself re-take or keep the token. Hence: **symmetric keep-token applies to
`token_handoff` ONLY; the three park sites void-release.**

- *No deadlock:* proven above — void-release is the non-deadlocking choice at a park site.
- *No lost wake:* the void-release is published under `g_sched_mutex` before any peer evaluates its
  predicate; R2's FREE-accept predicate catches it. The signal that ultimately wakes H is the object
  cv's own `notify` (Signal/Unlock/Set/drain), unaffected.
- *OFF byte-identical:* all three void-releases are inside `if (token_enabled() && holder==self)` /
  status blocks; OFF never enters them. This is exactly the ce95f13 behavior — v3 changes NOTHING at
  these three sites (it explicitly preserves them).
- *One-runnable:* preserved — at most the freed token is claimed by exactly one peer (R2 no-dup).

**Debug-assert invariant to add:** at the top of `select_next_runnable` and after each park-site
release, `assert(!(g_token_holder && g_token_holder->token_parked))` (ON-only; compile to no-op when
the flag path is dead). A `token_parked` thread is NEVER `g_token_holder`.

### C.R2 — FREE-token predicate: no duplication, no strand
Covered in detail in §B M5/F1 (R2 proof). Summary: the FREE-accept predicate only WAKES; the claim is
serialized under `g_sched_mutex` with the `while` re-check, so exactly one peer claims and the rest
re-park; the token is never left `nullptr` with all peers asleep because the void-release either
direct-grants a runnable peer (M2) or wakes a parked peer whose FREE predicate fires.

### C.R3 — SendMsgPipe wiring
- *No deadlock:* the producer void-releases the token to the consumer before parking on the
  full-pipe condition; the consumer runs `TryReceiveMsgPipe`, drains, notifies (cv form) or simply
  makes room (poll form); the producer's predicate/poll then succeeds and it reacquires. Without the
  wiring (ce95f13) the producer holds the token across the 5s spin and the consumer cannot run →
  guaranteed stall (R3's miss).
- *No lost wake:* cv form shares `g_msgpipe_mtx` between Send and TryReceive, notify-after-pop under
  the lock; poll form re-checks `mp.fifo.size()` under the lock each spin (1ms backstop). Either is
  race-free.
- *OFF byte-identical:* `sched_token_*` no-op OFF; the cv form still only parks when the pipe is full
  (never on Patapon's pipe), and the poll cadence (1ms) is unchanged OFF. State the cv addition as an
  OFF-observable structural change and confirm it never parks on Patapon.
- *One-runnable:* the producer hands to exactly the consumer (or any runnable peer) before parking.

### C.R4 — Exhaustive audit completeness
The whole `runtime/src` tree was grepped (§A.10 greps listed). Every `cv.wait*`, every
`this_thread::sleep_for`, every `for(;;)`/`while(true)`/spin/poll/retry, and every
MsgPipe/Fpl/Vpl/Mbx/Alarm/VTimer symbol is in the §A.10 table with a blocks-on-peer verdict and an
action. Real PSP-thread parks (#1-9): all wired after M1. Non-peer host sleeps (#11-13), render cvs
(#21-23), scheduler-internal/OFF-only cvs (#15-20), and the genuinely non-blocking TryReceive (#10)
and io leading-yields (#14): justified-out in §E. No FPL/VPL/Mbx/Alarm/VTimer/ReceiveMsgPipe blocking
handler exists in the runtime (grep returned zero) — forward note only.

### C.M1 (the wiring) — No deadlock: each wired park void-releases the token to a peer (or frees it)
BEFORE parking and reacquires after; the LockMutex valve only ADDS a wake. No lost wake: object cvs
still woken by Unlock/Set/Signal/drain `notify_*`; `release_for_wait` carries no wake obligation. OFF
byte-identical: all `sched_token_*` no-op OFF; the LockMutex valve and MsgPipe cv are the only
OFF-observable changes (call out, confirm against OFF gate). One-runnable: each wired park hands the
token to ≤1 peer (or frees it for exactly one claimant) before parking. **M1 makes the `token_parked`
exclusion and F3 SOUND** — the precondition.

### C.M2 — No deadlock: clearing `token_parked` earlier only makes a thread eligible sooner;
eligibility is gated by the predicate, not by `token_parked`. No lost wake: a thread is `token_parked`
only while on the foreign cv; once in `token_acquire` it is a valid grantee. OFF byte-identical:
`token_acquire` returns at the OFF gate before the clear matters. One-runnable: unchanged holder count.

### C.M4 — No deadlock: 100ms is a finite valve + `g_should_exit` escape. No lost wake: cv woken by
`SignalSema notify_all`. OFF byte-identical: ternary selects EXACTLY 5ms when `sched_token_enabled()`
is false. One-runnable: a timeout magnitude does not change ownership.

### C.M5/F1 — No deadlock: predicate evaluated under the lock the setter holds; the FREE-accept +
in-body claim recover both a granted notify and an R1 void-release. No lost wake: lost-wake window
closed (predicate-true returns immediately; void-release is predicate-true). OFF byte-identical:
returns at the OFF gate. One-runnable: only HOW the grantee wakes changes (R2: exactly one claims).

### C.F2 — No deadlock: under ON `release_for_wait` is the single hand-off; the removed leading yield
was redundant, never required. No lost wake: object-cv wake path unchanged. OFF byte-identical: gated
by `if (!sched_token_enabled())`. One-runnable: `release_for_wait` still hands to a peer before
parking.

### C.F3 — No deadlock: if no peer, keeping the token and continuing is the ONLY non-deadlocking
choice for a STILL-RUNNING holder (M1 ensures parked peers are invisible, so "no peer" means no one
else can run; a BLOCKING call always void-releases regardless of F3 — F3 is only on the yield path).
No lost wake: F3 adds/removes no cv op; a peer un-parking sets status/clears `token_parked` under
`g_sched_mutex` (M2), so the next scan sees it. OFF byte-identical: `token_handoff` gated; OFF never
enters the ON branch of `sched_yield_point`. One-runnable: strengthens it (never momentarily nulled
on the yield path).

### C.F4 — No deadlock: all remain finite valves + `g_should_exit`. No lost wake: notify/free-accept
driven; timeouts are backstops. OFF byte-identical: `SCHED_PREEMPT_BUDGET` only affects control flow
ON (`:386` gate); WaitSemaCB constant gated by M4. **CAUTION:** changing `SCHED_PREEMPT_BUDGET`
changes the numeric value written to `ctx->preempt_budget` even OFF (same arithmetic, different
constant, still never yields). Safe for the geometry/render gate; the verifier must confirm no
determinism gate hashes `recomp_context`. One-runnable: magnitudes don't affect ownership.

---

## D. S-prio — priority-inversion bound + REQUIRED verifier check

F3's "sole-runnable keeps the token" means a higher-priority peer that un-parks waits until the
holder's NEXT back-edge `sched_preempt` (now `other_runnable_exists == true` → hands off) OR until the
holder blocks. The worst-case wakeup latency is `SCHED_PREEMPT_BUDGET` back-edge passes of holder
guest code. **This is BOUNDED (not a hang) ONLY IF the holder's hot loops actually hit emitted
back-edge points.**

**REQUIRED verifier check (gate before accepting F3):** dump the emitted C++ for the .hack CRI-scanner
hot funcs **0x08839670, 0x0883B2B8, 0x08815E70, 0x08815EB8** and one Patapon hot loop, and confirm
`if (--ctx->preempt_budget <= 0) sched_preempt(ctx);` appears in the INNER loop body (use
`cargo run --release -- dump analysis.json 0xADDR`, grep `preempt_budget`). If any hot spin is
straight-line with no back-edge, no preemption point fires and F3 cannot hand off — that is an
emitter-coverage gap (a separate fix), not a token bug. (Emitter emits at loop back-edges,
`function.rs:706`; a `for`/`while` spin always has one, so this is expected to pass — but it MUST be
checked, not assumed.)

**Optional priority-aware forced-handoff (DEFER unless verification shows inversion):** a strictly-
higher-priority waiter that un-parks sets a flag (`g_higher_prio_waiting`) under `g_sched_mutex`; the
holder's `sched_preempt` forces a handoff regardless of budget. Specify but **DEFER** — only implement
if the S-prio check or the .hack/Patapon ON runs show a priority-inversion stall. With
`SCHED_PREEMPT_BUDGET=20000` the bound is already small.

---

## E. S-doc — park sites intentionally left un-wired (with justification)

- **`psp_render_queue.cpp:54,127` (`g_render_cv`, `g_ge_done_cv`) — RE-CLASSIFIED, NOW WIRED (M1.e).**
  The original v3 left these un-wired on the assumption they were render-thread-only. The final audit
  found both are reached on PSP GUEST threads (`g_current != null`): `render_queue_post` from the
  FramePresent/SetFrameBuf path and `render_queue_draw_sync` from `sceGeDrawSync`/`ListSync`. A PSP
  thread blocking there while holding the token starves peers for the whole GL frame and can wedge a
  token-mediated GE deadlock. Both cv waits are now bracketed with the token helpers (M1.e). The
  helpers still no-op on the actual render/main thread (`g_current == null`) and OFF.
- **`psp_ge_draw.cpp:873` (`g_ss_req_cv`)** and the MAIN-thread `render_queue_process` GE finish/signal
  callback (`g_current == null`): render/GE-thread protocol cvs reached on the main thread, never a PSP
  guest thread; the token helpers no-op there by design (a non-token thread must not take
  `g_sched_mutex` for token ops). **DEFERRED:** if the GE finish/signal callback fired from
  `render_queue_process` were itself to block, that block is on the main thread and is NOT covered by
  M1.e — a separate concern, flagged with a code comment, out of scope for this perf pass.
- **`DelayThread` (`kernel_thread.cpp:307`), `audio_block_for_samples` (`utility.cpp:188`), vblank
  (`display.cpp:57`)**: real-time host `sleep_for`, NOT object-cv peer parks. They hold the token
  across the sleep today; that is a pre-existing latency property, not a correctness bug under the
  invariant (no peer can be falsely granted — the sleeper is RUNNING-status but not `token_parked`, so
  a back-edge-less sleep holds the token for the sleep duration, and the leading `sched_yield_point()`
  before each already hands the token to a peer for the sleep — see below). Wiring them as scheduler-
  managed parks would change OFF timing. **Out of scope for this perf pass.** NOTE: each of these has
  a leading `sched_yield_point()` (DelayThread `:306`, audio `:187`, vblank `:54`); under ON that
  `token_handoff` already gives a peer the token before the real-time sleep, so the sleep does NOT
  starve peers in the common multi-runnable case — F3 only keeps the token when sole-runnable, in
  which case the sleep is harmless. These leading yields are NOT gated by F2 (they precede a real-time
  sleep, not an object-cv park).
- **`psp_hle_io.cpp` (all `sched_yield_point` sites)**: leading cooperative yields only — no cv, no
  sleep, no spin park (grep confirmed zero `cv.`/`sleep_for`/`for(;;)`/`while(true)` in io.cpp). The IO
  path does its work synchronously on the host fd and returns; nothing blocks on a PSP peer. Left as-is
  (the yields are existing fairness points, not token parks). NOT gated by F2.
- **`TryReceiveMsgPipe` (`utility.cpp:1734-1775`)**: genuinely non-blocking (single lock, returns
  MPP_EMPTY when short). No park. Left un-wired. There is no blocking `sceKernelReceiveMsgPipe`.
- **`psp_scheduler.cpp:226` (`thread_entry` gate), `:339` (OFF yield path), `:493` (shutdown join
  sleep)**: not guest object parks; the OFF path must stay byte-identical.
- **No FPL/VPL/Mbx/Alarm/VTimer blocking handlers exist** (grep returned zero registered handlers).
  Forward note: if added, wire per M1.

---

## F. Summary of edits (all ON-only behind the gate; OFF byte-identical except M1.b valve + M1.d cv)

- `psp_scheduler.h`: add `bool sched_token_enabled();` decl; `SCHED_PREEMPT_BUDGET` 100000→20000 (F4).
- `psp_scheduler.cpp`: `sched_token_enabled()` def + `other_runnable_exists()` (F3); INVARIANT comment
  + debug-assert above `select_next_runnable` (M1/R1); move `token_parked=false` to top of
  `token_acquire` + `holder==self` guard (M2); predicated wait with FREE-accept + valve counters
  (M5/F1, R2); F3 guard in `token_handoff`. **The three park sites (`release_for_wait` `:411-415`,
  `sleep_current` `:565-568`, `wait_end` `:658-661`) are UNCHANGED — keep the void-release (R1).**
- `psp_hle_kernel_sema.cpp`: wire deleted-path 100ms park (M1.a); WaitSemaCB 5ms→ternary 100ms/5ms
  gated (M4); gate leading `sched_yield_point()` at `:236` (F2).
- `psp_hle_kernel_eventflag.cpp`: gate leading `sched_yield_point()` at `:164` (F2).
- `psp_hle_kernel_mutex.cpp`: wire LockMutex park + finite 5s valve + `g_should_exit` escape (M1.b);
  gate leading `sched_yield_point()` at `:58` (F2).
- `psp_hle_kernel_lwmutex.cpp`: bracket `lw_lock_blocking` for-loop with release/reacquire (M1.c);
  gate leading `sched_yield_point()` at `:283` (F2).
- `psp_hle_utility.cpp`: wire SendMsgPipe full-pipe wait (M1.d, R3) — **implemented as the fallback
  form** (release/reacquire around the existing `sleep_for(1ms)`; no `<condition_variable>` dep, no
  `PspMsgPipe` cv member, OFF poll cadence unchanged at 1ms); gate leading `sched_yield_point()` at
  `:1693` (F2). TryReceiveMsgPipe unchanged.
- `psp_render_queue.cpp`: bracket `render_queue_post`'s `g_render_cv.wait` and
  `render_queue_draw_sync`'s `g_ge_done_cv.wait_for` with release/reacquire (M1.e); leading `:119`
  yield left in place; DEFERRED main-thread GE-callback comment added at `render_queue_process`.
- NO emitter change. Verifier runs the S-prio back-edge dump check.

---

## Resolved review findings (item → where addressed)

| Item | Addressed in |
|---|---|
| **R1** DROP M3; three park sites keep void-release; symmetric keep-token = `token_handoff` only; debug-assert `token_parked` ⇒ never `g_token_holder` | §A.2/§A.7/§A.8 (void-release mapped), §B (M3 absent; §F notes "unchanged"), §C.R1 (deadlock proof + assert) |
| **R2** predicate accepts FREE token; keep nullptr-claim; no dup / no strand | §B M5/F1 (final body + R2 proof), §C.R2 |
| **R3** WIRE SendMsgPipe full-pipe park; gate its leading yield; check TryReceive/ReceiveMsgPipe | §A.10 #9/#10, §B M1.d, §C.R3, §E (TryReceive out, no ReceiveMsgPipe) |
| **R4** exhaustive runtime-wide re-audit; complete site table | §A.10 (23-row table, whole tree), §C.R4, §E |
| **M1** wire mutex/lwmutex/WaitSema-deleted + LockMutex finite valve + MsgPipe + invariant + un-wired list | §A.10, §B M1.a/.b/.c/.d, §E |
| **M2** clear `token_parked` at TOP of `token_acquire`; soundness argument | §B M2, §C.M2 |
| **M4** gate WaitSemaCB timeout (ternary 100/5ms); add public `sched_token_enabled()` | §B M4, §F |
| **M5 (=F1)** predicated `token_acquire` wait + FREE-accept + kept nullptr-claim + ON-only counters | §B M5/F1, §C.M5/F1 |
| **F2** gate leading `sched_yield_point()` (eventflag/sema/mutex/lwmutex/msgpipe) behind `!sched_token_enabled()` | §B F2, §E (which yields NOT gated + why) |
| **F3** sole-runnable fast path via `other_runnable_exists`; now safe (M1+M2); `token_handoff` only | §B F3, §C.F3 |
| **F4** budget 100k→20k; keep 50ms backstop; WaitSemaCB→100ms gated; one-constant-per-run | §B F4, §D, §B M4 |
| **S-prio** inversion bound + REQUIRED back-edge verifier check; optional forced-handoff DEFERRED | §D |
| **S-doc** intentionally un-wired park sites + why (render, host sleeps, io, TryReceive, scheduler-internal) | §A.10 notes, §E |
| **M1.e** (final-audit must-fix) wire render-queue PSP-thread parks (`render_queue_post`, `render_queue_draw_sync`); defer main-thread GE callback | §A.10 #21/#22, §B M1.e, §E |

---

## Implementation notes (as built — `feat/single-runnable`)

What was actually implemented in this pass, and where the build deviates from the spec text:

- **M1.d implemented as the FALLBACK form** (not the preferred cv conversion). The fallback wraps the
  existing `sleep_for(1ms)` in `sched_token_release_for_wait()`/`_reacquire_after_wait()`. Rationale:
  the cv form needs a new `<condition_variable>` include and a `PspMsgPipe::not_full` member plus a
  lock-held-across-wait restructure (the current code releases `g_msgpipe_mtx` between spins via an
  inner scope) — strictly higher risk for no OFF benefit (Patapon's pipe never fills). The fallback is
  OFF byte-identical (1ms poll cadence unchanged; both helpers no-op OFF) and makes SendMsgPipe a sound
  park under the §A.10 invariant. If a future title fills the pipe hot, revisit the cv form.
- **M1.e added** (was not in v3): render-queue present + DrawSync parks are reached on PSP threads and
  are now bracketed. See §B M1.e / §E.
- **M2 guard `if (g_token_holder == self) return;`** is KEPT and load-bearing: it recovers a grant
  delivered to `self` during the `thread_entry` startup window (or any prior race) where `self` may
  already be the holder. It is NOT the dropped M3 keep-token path — it is a cheap correctness guard for
  an already-granted holder (comment says so in `token_acquire`).
- **Valve counters** (`g_valve_timeout_wakes`, `g_valve_nullptr_claims`) are file-static
  `std::atomic<uint64_t>`, incremented ONLY inside `token_acquire` after its OFF gate, so OFF never
  touches them. They are currently not yet surfaced via the debug socket / a periodic stderr line
  (increment-only); wiring a readout is a trivial follow-up if the acceptance run needs the signal.
- **Debug-assert** uses `<cassert>` `assert()` at the top of `select_next_runnable`; compiles out under
  `NDEBUG` (Release) and is unreachable OFF (`g_token_holder` stays `nullptr`).
- **The three R1 park sites are byte-for-byte UNCHANGED** (`release_for_wait`, `psp_thread_sleep_current`,
  `psp_thread_wait_end` keep `g_token_holder = nullptr` void-release). Verified no edit touched them.

## Deferred (not in this pass)

- **Main-thread GE finish/signal callback blocking** (`render_queue_process`, `g_current == null`): if
  that callback blocks, the block is on the main thread and is NOT covered by M1.e's PSP-thread wiring.
  Flagged with a code comment at `render_queue_process`; needs a different mechanism (not the token
  helpers, which correctly no-op off a non-PSP thread). Out of scope.
- **Real-time host sleeps hold the token = residual ON latency.** `DelayThread`
  (`kernel_thread.cpp`), `audio_block_for_samples` (`utility.cpp`), and vblank (`display.cpp`) are
  real-time `sleep_for`s, not object-cv peer parks. Each has a LEADING `sched_yield_point()` that hands
  the token to a peer before the sleep in the multi-runnable case, but when the sleeper is the SOLE
  runnable it keeps the token across the sleep (F3). This is a pre-existing latency property, not a
  correctness bug under the invariant; wiring them as scheduler-managed parks would change OFF timing.
  Deferred per §E.
- **M5/F1 valve-counter readout** (debug socket / periodic stderr): counters exist but are not yet
  exposed. Trivial follow-up.
- **S-prio optional priority-aware forced-handoff** (`g_higher_prio_waiting`): specified in §D, DEFERRED
  unless the .hack/Patapon ON runs show a priority-inversion stall. With `SCHED_PREEMPT_BUDGET=20000`
  the inversion bound is already small.
- **F4 staged constant tuning**: this pass lands `SCHED_PREEMPT_BUDGET=20000` + WaitSemaCB=100ms-ON;
  per-run bisection of the budget (10k/5k) against the .hack freeze-rate harness is the verifier's job,
  not this pass.
