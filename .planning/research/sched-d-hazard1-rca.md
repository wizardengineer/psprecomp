# sched-d Hazard 1 — token hand-off ordering race: RCA + generic fix design

**Status:** diagnosis + design, read-only pass on `feat/single-runnable @ e9d0c96`. Live-reproduced
the Patapon ON livelock, pinned the exact mechanism with the sema-271 trace + a code-level
lock-ordering proof, confirmed the same root wedges `.hack`'s CRI ring, and designs ONE generic,
OFF-byte-identical fix that restores faithful PSP single-CPU ordering without reverting to the slow
`ce95f13` checkpoint. Integrates Hazard 2 (GE-callback Option C from
`sched-d-ge-callback-rca.md`) coherently.

**Headline (load-bearing, sharper than the prior hypothesis):** The dominant blocker is NOT a
"token bounces between two threads" livelock and NOT a lost cv-notify. It is a **lock-ordering
deadlock** that the perf pass introduced by calling the *blocking* `sched_token_reacquire_after_wait()`
**while the HLE wait still holds the object mutex** (`s->mtx` / `ef->mtx`). A thread that re-locks the
object mutex on cv return and then **parks inside `token_acquire`** strands the object mutex; the
only peer that could free the run-token first needs that same object mutex to do its work (or even
just to run its own WaitSemaCB acquire). F3 (sole-runnable keep-token) and the DelayThread
keep-token-while-sleeping are the *amplifiers* that make the stranding permanent and
non-deterministic, but the *primary* defect is **object-mutex held across a blocking token
re-acquire**. The fix must move the blocking token re-acquire OUT from under the object mutex (plus
constrain F3 / add a needed-peer forced hand-off). This is exactly the producer/consumer single-CPU
ordering class `.hack` shares, so the fix unblocks both games.

---

## 1. Captured wedge state (live, e9d0c96, `runtime/build/`, Patapon BOOT.BIN)

ONE global runtime; `pgrep` first; debug socket TCP 9999; radioactive logs (timeout → grep →
delete); `kill <pid>` not `pkill`. `PSPRECOMP_PREEMPT=1 PSPRECOMP_CROSS_MID=1`.

### 1.1 Steady wedge (deterministic-to-black)
Socket `I`, three snapshots over ~10 s (uptime 14.9 → 40.6 → 44.7 s):
```
ge = {frames:1, prims:0, real_nonsprite:0, sprite_nonclear:0, clears:0}
threads = [
  {id:0, name:"user_main",  status:"RUNNING", wait:"semacb:271"},   // uid 256, pri 32 (geometry driver)
  {id:1, name:"FileThread", status:"RUNNING", wait:"semacb:271"}    // uid 270, pri 111 (IO pump)
]
recent_funcs head = ["0x088648E4","0x0886095C","0x088648E4","0x08861C68","0x08861AEC","0x08861C68",...]
```
`recent_funcs` head is **byte-identical** across all three snapshots 4 s apart → no guest forward
progress. The 6 `sgx-psp-*` audio threads never spawn. Both PSP threads are in the **WaitSemaCB(271)
poll loop** (`wait="semacb:271"`). Log **stops growing** (387 lines, frozen).

### 1.2 The decisive sema-271 trace (`PSPRECOMP_SEMA_TRACE=1`)
271 is a **guest binary mutex** (count-1). Guest `FUN_0886207C` is a lock/work/unlock wrapper
(verified by `dump`): `if (lock_flag) WaitSemaCB(obj+56=271,1,0)` → inner work `FUN_088620E8`
(drains the IO ring via `FUN_08862234`×3) → `if (lock_flag) SignalSema(obj+56=271,1)`. The 271
trace pairs cleanly lock→work→unlock throughout and the **LAST event before the wedge is:**
```
[SEMA271] WaitSemaCB acquired count_after=0 caller=0x08861850
[SEMA271] Signal count=0->1          caller=0x08861860     <-- 271 LEFT AT count=1 (FREE/unlocked)
```
**271 is FREE at the wedge.** It is not held. Both threads are spinning in `WaitSemaCB(271)` whose
acquire predicate is `current_count >= signal && s->waiters.empty()` — and the CB poll loop does
**NOT** push to `s->waiters` (it only bumps `wait_count`; only the blocking `WaitSema` path at
`sema.cpp:324` pushes). So with `current_count==1, signal==1, waiters.empty()==true`, **any thread
that re-runs one WaitSemaCB poll iteration would acquire 271 and progress.** Neither does. The wedge
is therefore **run-token / lock starvation, not a guest deadlock** (the guest mutex is available).

### 1.3 IO pipeline position at the wedge
The IO async callback chain DID fire (`notify_callback ×6`, `sceIoCloseAsync(fd=3)`, state machine
reached `[SM_POST] new_state=300`), the GE finish-cb fired once (`finish-cb fn=0x08816F9C ×1`,
`token=0x0000` → off-token, identical to OFF), then everything stopped. `SignalSema 271` total in
the wedge run with state-machine progress = the trace above (last = free). `First PRIM ×0`,
`SetFrameBuf ×0`. This matches `sched-d-ge-callback-rca.md` §1.2 exactly (its run-1 wedge), and that
RCA's Hazard-1 pin ("the consumer that runs the ring never advances under ON").

---

## 2. The proven mechanism — code-level interleaving

### 2.1 The primary defect: object mutex held across a BLOCKING token re-acquire

`hle_sceKernelWaitSemaCB` poll-loop body (`runtime/src/hle/psp_hle_kernel_sema.cpp:445-481`):
```cpp
{
    std::unique_lock<std::mutex> lock(s->mtx);                       // (L1) take object mutex 271
    if (s->current_count >= signal && s->waiters.empty()) { ...return SCE_OK; }   // fast acquire
    s->wait_count++;
    sched_token_release_for_wait();                                 // (L2) g_sched_mtx; token_parked=true; void-release
    s->cv.wait_for(lock, 100ms, pred);                             // (L3) drop s->mtx, park; RE-LOCK s->mtx on return
    sched_token_reacquire_after_wait();   // -> token_acquire(...)  // (L4) BLOCKS until token granted/free
    s->wait_count--;
}                                                                   // (L5) s->mtx released HERE (end of scope)
```
`std::condition_variable::wait_for` **re-acquires `s->mtx` before returning** (L3 end). So at **L4**
the thread holds `s->mtx` AND calls `sched_token_reacquire_after_wait()` →
`token_acquire(lock=g_sched_mutex, self)`. `token_acquire` (`psp_scheduler.cpp:164-181`) **parks on
`self->cv`** (releasing only `g_sched_mutex`, NOT `s->mtx`) whenever `g_token_holder` is some third
thread (≠ self, ≠ nullptr). **Result: a thread blocked waiting for the run-token while holding the
guest mutex's host object mutex `s->mtx`.** `s->mtx` is released only at L5, which is unreachable
until the token is granted.

This is the lock-ordering inversion the design swore off, hidden: the design's invariant is
`object_mtx → g_sched_mutex` for the *transient* helpers, which holds *lexically*. But `token_acquire`
is not transient — it **blocks**. Holding `object_mtx` across a *blocking* `g_sched_mutex`-mediated
wait is a deadlock vector the design's "held transiently" assumption (perf-design §A.0) does not
cover.

### 2.2 The two-thread wedge (both in WaitSemaCB(271))

Let A = `user_main` (pri 32), B = `FileThread` (pri 111). Both are in `WaitSemaCB(271)`.

1. A holds the token, runs its poll: count<1, so A does L2 (release_for_wait: `A.token_parked=true`;
   A was holder so `g_token_holder=nullptr` then `token_release_to(A)`). The only peer B is
   `token_parked` (B is parked in B's own L3 `s->cv.wait_for`), so `select_next_runnable(A)` returns
   nullptr → **token left `nullptr` (free)**. A parks at L3 on `s->cv` (dropping `s->mtx`).
2. SignalSema(271) somewhere sets count 0→1 and `notify_all`s `s->cv`. **Both A and B wake from L3**
   (predicate `count>=signal && waiters.empty()` is now TRUE for both). Each re-locks `s->mtx` on
   return — but `s->mtx` is a single mutex, so **exactly one** (say B) gets `s->mtx` first; A's L3
   return blocks re-locking `s->mtx`.
3. B (holding `s->mtx`) runs L4 `token_acquire`: `g_token_holder==nullptr` → R2 first-free/claim →
   `g_token_holder=B`, returns. B exits the inner scope (L5 releases `s->mtx`), loops to the top,
   re-takes `s->mtx`, **acquires 271** (count 1→0), returns SCE_OK. B makes progress. **GOOD so
   far** — this is the path that lets the wedge sometimes get "further" (RCA run-2).
4. **The wedge variant:** B, now holding the token, proceeds into guest work that calls
   `hle_sceKernelDelayThread` (`kernel_thread.cpp:296`) — the FileThread IO pump
   (`FUN_08862C14`) calls `sceKernelDelayThreadCB`/`sceIoPollAsync` in a loop. DelayThread does
   `psp_kernel_check_callbacks` → `sched_yield_point()` (→ `token_handoff`) → **`sleep_for(usec)`**.
   At `token_handoff`, **F3** (`psp_scheduler.cpp:208`) sees `other_runnable_exists(B)`: A is
   `token_parked` (still parked in its L4/L3) → invisible → F3 returns **keeping the token**. B then
   `sleep_for`s **holding the token**. Now A, which was about to re-lock `s->mtx` at its L3 return,
   gets `s->mtx`, runs L4 `token_acquire`: `g_token_holder==B` (≠ A, ≠ nullptr) → A's predicate is
   FALSE → **A parks in token_acquire holding `s->mtx`** (the §2.1 deadlock).
5. Now: A is blocked in `token_acquire` **holding `s->mtx`**, waiting for the token B holds. B wakes
   from `sleep_for`, loops the IO pump, and on its next `WaitSemaCB(271)` poll or any path that
   touches sema 271 it does L1 `std::unique_lock<std::mutex> lock(s->mtx)` — **which A holds** → **B
   blocks on `s->mtx`.** B never reaches a release point for the token. A waits for the token; B
   waits for `s->mtx`. **Permanent deadlock.** (Even the 50 ms token valve and 100 ms cv valve do
   not break it: A's `token_acquire` wakes every 50 ms but `g_token_holder` is still B, so A
   re-parks *still holding `s->mtx`*; B is blocked on `s->mtx` at L1, not on a cv with a valve.)

The non-determinism (RCA run-1 early stall vs run-2 further) is exactly which thread wins the
`s->mtx` race at step 2/3 and whether B's DelayThread keep-token (F3) fires before or after A
re-grabs `s->mtx` — a scheduling-order race, the fingerprint the single-runnable token was meant to
*remove*.

### 2.3 Which candidate mechanism fires — (a), (b), or (c)?

The task framed three candidates. The verdict, from the evidence:

- **(a) F3 keep-token-while-the-consumer-is-parked: FIRES, as the amplifier.** F3 lets B keep the
  token across `sleep_for` (DelayThread) and across straight-line IO-pump work while A is
  `token_parked`/blocked. Without F3, B's `sched_yield_point` at DelayThread would `token_release_to`
  — but `select_next_runnable` would *still* return nullptr (A is `token_parked`), so the token would
  be void-released to `nullptr` and A's R2 free-claim could then take it. **So F3 is not the whole
  story** — even with F3 removed, the §2.1 stranded-`s->mtx` deadlock (step 4-5) still forms because
  the defect is A blocking in `token_acquire` under `s->mtx`, independent of F3.
- **(b) R2 first-free / g_rr_cursor picks the wrong next-runnable: PARTIALLY — it is the
  non-determinism source, not the deadlock.** R2's "whichever peer's free-accept predicate fires
  first claims" decides *which* of A/B wins at step 3, and `g_rr_cursor` does not reproduce PSP's
  real priority order (it never even runs here because both peers are `token_parked` so
  `select_next_runnable` returns nullptr and the *free-claim* path, not the directed grant, is what
  acts). So (b) explains *why the wedge is non-deterministic and priority-blind*, but the hang itself
  is the §2.1 lock-order strand, not a wrong RR pick.
- **(c) missed/late hand-off: this is the precise root, sharpened.** The "late hand-off" is exactly
  the §2.1 structure: A becomes runnable (its sema predicate is satisfied: 271 free) but it can never
  *act* on that because it is wedged in `token_acquire` holding `s->mtx`, and B can never *yield* the
  token because B is wedged on `s->mtx`. It is not "F3's check ran a moment too early" — it is "the
  blocking token re-acquire sits under the object mutex, so the two threads form a classic AB-BA wait
  (A: has s->mtx, wants token; B: has token, wants s->mtx)."

**Proven dominant mechanism: a hybrid (c)+(a).** The *deadlock structure* is (c): object mutex
(`s->mtx`/`ef->mtx`) held across the blocking `token_acquire` → AB-BA wait between "has-mutex-wants-
token" and "has-token-wants-mutex". F3 (a) and DelayThread-keep-token are the amplifiers that make
the token-holder B run long enough / sleep holding the token so A reliably reaches the
stranded-`s->mtx` state; R2/`g_rr_cursor` (b) make it non-deterministic and priority-blind. The fix
must address the (c) lock-order strand FIRST (it is the actual hang); constraining F3 and adding a
forced hand-off remove the amplifier and the priority-inversion.

### 2.4 Why OFF does not hit this (and must not change)
OFF: `sched_token_release_for_wait`/`_reacquire_after_wait` are no-ops (`token_enabled()==false`,
`psp_scheduler.cpp:493,508`). So L2/L4 do nothing; `s->mtx` is held only across L3's `cv.wait_for`
(which *releases* it during the park) — the normal, deadlock-free pattern. No thread ever blocks in
`token_acquire`, so no thread strands `s->mtx`. OFF renders perfectly (`ge.frames=250, prims=3977`).
**The fix must keep every OFF path a literal no-op.**

---

## 3. Does the same root wedge `.hack`? YES.

`dothack-same_root = true.` The runtime core (`psp_scheduler.cpp`, `psp_hle_kernel_sema.cpp`,
`psp_hle_kernel_eventflag.cpp`) is **generic, shared by every `PSPRECOMP_GAME`** (Patapon and the
`-DPSPRECOMP_GAME=none` `.hack` build both link the same files). `.hack`'s CRI ring hand-off uses
**event flags** (`SetEventFlag CriCond uid 265/268/271`, per `dothack-ring-ordering.md` §3a) and a
CpuSuspend/Resume critical section the CRI scanner busy-polls. The eventflag wait path has the
**identical §2.1 hazard structure** (`psp_hle_kernel_eventflag.cpp:189-203`):
```cpp
std::unique_lock<std::mutex> lock(ef->mtx);            // take object mutex
ef->num_wait_threads++;
sched_token_release_for_wait();
ef->cv.wait_for(lock, 5s, pred);                       // re-locks ef->mtx on return
sched_token_reacquire_after_wait();                    // BLOCKS in token_acquire holding ef->mtx
ef->num_wait_threads--;
```
A `.hack` consumer parked here re-locks `ef->mtx` on the CriCond notify, then blocks in
`token_acquire` **holding `ef->mtx`**; the producer's `SetEventFlag` takes `ef->mtx` to OR the
pattern + `notify_all` (`psp_hle_kernel_eventflag.cpp` signal path) → **the producer blocks on
`ef->mtx`** that the token-starved consumer holds. Same AB-BA wait, same F3 amplifier
(`dothack-ring-ordering.md` §1: `user_main` RUNNING busy-poll keeps the token while CriThreads are
parked). The `.hack` freeze rate ~50% is the non-deterministic `s->mtx`/`ef->mtx`-race signature. The
prior `.hack` doc correctly diagnosed "single-CPU ordering of the cross-thread hand-off" as the cause
and named approach (d) as the fix — but (d)'s *current* implementation re-introduces the very
ordering defect via the under-mutex blocking re-acquire. **Fixing Hazard 1 here is necessary for
`.hack` to progress; the GE-callback (Hazard 2) fix alone is not sufficient for either game.**

---

## 4. Option analysis (restore PSP single-CPU ordering WITHOUT the slow checkpoint)

Goal: deadlock-free; OFF byte-identical (OFF currently PASSES — do not touch it); exactly-one-guest-
runs preserved; generic (271/`FUN_0886207C` are guest facts, never hardcoded).

### Option 1 — Move the blocking token re-acquire OUT from under the object mutex (PRIMARY, REQUIRED)
The §2.1 deadlock exists because `sched_token_reacquire_after_wait()` (which blocks) is called while
`s->mtx`/`ef->mtx` is held. Restructure every wired HLE wait so the object mutex is **dropped before**
the blocking token re-acquire, then **re-taken after** to re-check the guest condition:
```cpp
{ std::unique_lock<std::mutex> lock(s->mtx);
  if (acquire_ok()) { ...return; }
  s->wait_count++;
  sched_token_release_for_wait();                 // unchanged: parked, void-release
  s->cv.wait_for(lock, valve, pred);              // park on object cv
  s->wait_count--;
}                                                 // <-- DROP s->mtx HERE
sched_token_reacquire_after_wait();               // block for the token with NO object mutex held
// loop: re-take s->mtx at the top and re-check acquire_ok()
```
This is the disciplined `object_mtx → g_sched_mutex` order the design intended — the helpers take
`g_sched_mutex` with **no** object mutex held, so a blocking `token_acquire` can never strand the
object mutex. The peer (`SignalSema`/`SetEventFlag` producer, or the peer's own WaitSemaCB acquire)
always finds the object mutex free. **This alone removes the deadlock** (the AB-BA wait cannot form:
no thread holds the object mutex while waiting for the token).
- *Pro:* directly kills the proven root; tiny, mechanical, per-site; preserves the wake correctness
  (the cv predicate already re-checks on the next loop iteration after re-acquire).
- *Con:* a TOCTOU window between dropping `s->mtx` and re-taking it: the count could change. Handled
  by the existing **outer poll loop** (WaitSemaCB already loops and re-checks `acquire_ok()` under
  `s->mtx` at the top — `sema.cpp:449`). For the eventflag/sema-blocking/mutex/lwmutex paths that do
  **not** already outer-loop, wrap the release/wait/reacquire/re-check in a small `while
  (!acquire_ok() && !g_should_exit)` so the dropped-mutex window is re-validated. This is the same
  shape as `M1.b`'s finite-valve loop, generalized.

### Option 2 — Constrain F3 (the amplifier)
With Option 1 the deadlock is gone, but F3 still lets a sole-runnable holder keep the token across
`sleep_for`/straight-line work while a peer is `token_parked` waiting on a signal that holder will
eventually emit — a *throughput* / *priority-inversion* hazard (the holder runs to its next back-edge
before the higher-priority woken peer gets a turn). Two sub-options:
- **2a (recommended): keep F3, add a NEEDED-PEER forced hand-off.** When a peer transitions to
  runnable (un-parks: `reacquire_after_wait`'s `token_parked=false`; resume; the producer's
  Signal/Set wake), set a flag `g_handoff_requested=true` under `g_sched_mutex`. The holder's next
  `sched_preempt` back-edge (and the DelayThread `sched_yield_point`) honors it: if
  `g_handoff_requested`, force `token_release_to` regardless of the F3 sole-runnable check. Bounds
  the inversion to one `SCHED_PREEMPT_BUDGET` window (already the design's §D bound) and guarantees a
  just-woken higher-priority peer gets the CPU promptly.
- **2b: disable F3 on the sleep/WaitSemaCB path only.** Make `hle_sceKernelDelayThread`'s
  `sched_yield_point` (and the WaitSemaCB leading path) always void-release instead of F3-keep. Less
  general; 2a is preferred because it is one mechanism that also serves Hazard 2 (§6).

### Option 3 — Fix R2 / g_rr_cursor ordering
After Option 1+2 the token is granted via `token_release_to` → `select_next_runnable` (priority +
RR) in the common case; the R2 free-claim becomes a rare backstop. `g_rr_cursor` already implements
within-priority RR and `select_next_runnable` already picks lowest-priority-number first, which
reproduces PSP's priority-preemptive + RR order **once peers are no longer spuriously excluded**.
The real R2 ordering bug today is that BOTH peers are `token_parked` (so the directed grant finds
nobody and the free-claim races). Option 1 fixes the upstream cause (a peer un-parks cleanly and
becomes a directed-grant target via M2). **No separate R2 rewrite is needed**; verify post-fix that
`g_valve_nullptr_claims` stays small-and-bounded (design's acceptance signal) — a high rate would
signal a residual missed direct-grant.

### Option 4 (FALLBACK) — Drop F3 entirely, recover perf via M5/F1 + coarser quantum
If Option 2a proves fragile, drop F3 and rely on the M5/F1 futex-style predicated wake + a coarser
`SCHED_PREEMPT_BUDGET`. **Perf cost vs the slow checkpoint:** the slow `ce95f13` checkpoint collapsed
because every park fell to the 50 ms `token_acquire` valve and 5 ms WaitSemaCB poll. M5/F1 already
removes the 50 ms-valve dependence (predicated wake fires on the grant notify, not the timeout), so
dropping F3 costs only the release/reacquire round-trip on each sole-runnable yield (one 64-slot
scan + a no-op grant), **not** the throughput collapse. Estimated: noticeably slower than F3-on
(every DelayThread/back-edge does a full hand-off round-trip even when sole-runnable), but **far
faster than the slow checkpoint** (no valve-latency dependence). Acceptable as a safety net; not the
first choice because Option 2a keeps F3's win and bounds the inversion.

---

## 5. Recommended fix (Option 1 + Option 2a; Option 3 verified-not-rewritten)

**`f3_verdict = keep-constrained.`** Keep F3 (its sole-runnable fast path is a real win and is *not*
the deadlock — the under-mutex blocking re-acquire is). Constrain it with a needed-peer forced
hand-off (2a). The deadlock itself is removed by Option 1 (drop the object mutex before the blocking
token re-acquire), which is the load-bearing change.

### 5.1 Fix sketch — site-by-site (all ON-only behind `sched_token_enabled()`; OFF literal no-op)

**(F1) Drop the object mutex before the blocking token re-acquire — every wired HLE wait.**
Sites (object mutex → blocking `reacquire` to un-nest):
- `psp_hle_kernel_sema.cpp:446-481` WaitSemaCB poll loop — move `sched_token_reacquire_after_wait()`
  to AFTER the `{ ... }` scope that holds `s->mtx`; the existing outer `while` re-checks the acquire
  under `s->mtx` at the top (`:449`), so the TOCTOU window is already covered. Net: `release_for_wait`
  and `cv.wait_for` stay under `s->mtx`; `reacquire_after_wait` runs with NO mutex held.
- `psp_hle_kernel_eventflag.cpp:189-203` WaitEventFlag — wrap in a `while (!pattern_matches && !exit)`
  loop; `release_for_wait` + `cv.wait_for` under `ef->mtx`, then drop `ef->mtx`, then
  `reacquire_after_wait`, then re-take `ef->mtx` at loop top and re-check the pattern.
- `psp_hle_kernel_sema.cpp:280-285` (WaitSema deleted), `:337-377` (WaitSema blocking FIFO) — same:
  reacquire after dropping `s->mtx`. For the FIFO path, the waiter is granted via `sema_grant_waiters`
  setting `w->granted`; re-check `w->granted` under `s->mtx` after the token reacquire.
- `psp_hle_kernel_mutex.cpp:89-94`, `psp_hle_kernel_lwmutex.cpp:154-201` — drop `m->mtx`/`g_lw_mtx`
  before `reacquire_after_wait`, re-take + re-check `acquired()` at loop top.
- `psp_hle_utility.cpp:1739-1741` (SendMsgPipe), `psp_render_queue.cpp:60-64,141-145` — same pattern:
  the blocking `reacquire` must not nest under the object/render mutex.

The single rule, statable as the new invariant comment: **`sched_token_reacquire_after_wait()` is
ALWAYS called with NO object/render mutex held; only `sched_token_release_for_wait()` and the
`cv.wait_*` may run under the object mutex** (release is non-blocking — it takes `g_sched_mutex`
transiently and returns; only the *re-acquire* can block, so only the re-acquire must be un-nested).

**(F2) Needed-peer forced hand-off (2a).** In `psp_scheduler.cpp`:
```cpp
static bool g_handoff_requested = false;   // guarded by g_sched_mutex (ON-only)

// In token_acquire, AFTER self->token_parked=false (M2), when a peer becomes a valid
// grantee by re-contending, request a hand-off from the current holder:
//   if (g_token_holder != self && g_token_holder != nullptr) g_handoff_requested = true;
// In token_handoff (F3 site), honor it BEFORE the sole-runnable short-circuit:
static void token_handoff(std::unique_lock<std::mutex>& lock, PspThread* self) {
    if (!token_enabled() || g_token_holder != self) return;
    if (!g_handoff_requested && !other_runnable_exists(self)) return;  // F3, now needed-peer aware
    g_handoff_requested = false;
    token_release_to(self);
    if (g_token_holder == self) return;
    token_acquire(lock, self);
}
```
And in `hle_sceKernelDelayThread`, the `sched_yield_point()` at `:306` already routes through
`token_handoff`, so the forced hand-off makes B yield to a just-woken A before `sleep_for` — closing
the keep-token-while-sleeping amplifier. `g_handoff_requested` is set under `g_sched_mutex`, cleared
on the hand-off, and is **dead OFF** (only token_acquire/token_handoff touch it, both gated).

### 5.2 Lock & token ordering after the fix
`object_mtx → g_sched_mutex` is preserved AND strengthened: `release_for_wait` takes `g_sched_mutex`
under `object_mtx` (transient, non-blocking, returns) — fine; `reacquire_after_wait` takes
`g_sched_mutex` with **no** `object_mtx` held (F1) — so the blocking wait never nests. No cycle: the
only place `object_mtx` and `g_sched_mutex` are both held is the transient non-blocking
`release_for_wait`, and it always releases `g_sched_mutex` before returning to the object-cv park.

---

## 6. Hazard 2 (GE-callback Option C) integration — they land together

Hazard 2's Option C (`sched-d-ge-callback-rca.md` §5) adds a "GE interrupt window" so the
main/render thread runs the off-token GE finish/signal callback as the sole guest executor:
`sched_ge_interrupt_enter()` waits for `g_token_holder == nullptr`, sets `g_ge_intr_active`, runs the
callback, `sched_ge_interrupt_leave()`. Its **open concern #2** is precisely the F3 problem: *"if a
PSP thread busy-spins sole-runnable and keeps the token via F3, the GE-interrupt window may never
open."* The **needed-peer forced hand-off (F2 here) is the shared mechanism that resolves it**:
- `sched_ge_interrupt_enter()` sets the same `g_handoff_requested = true` (a GE interrupt is a
  needed peer) before waiting on `g_token_holder == nullptr`. The current holder's next
  `sched_preempt`/`sched_yield_point` back-edge honors it (F2) → void-releases the token →
  `g_token_holder == nullptr` → the GE-interrupt window opens in bounded time (one
  `SCHED_PREEMPT_BUDGET` window), instead of waiting for the holder to happen to block.
- Both land as ONE coherent change: F2 introduces `g_handoff_requested` + the honor-point in
  `token_handoff`; Option C's `enter` reuses it. The `token_acquire` extra condition (a peer may not
  take the free token while `g_ge_intr_active`) and the F1 un-nesting are orthogonal and compose
  (F1 ensures no object mutex is stranded when the GE window wants the CPU free).

**Landing order:** F1 (un-nest the blocking re-acquire) FIRST — it removes the deadlock and is the
precondition for any forced hand-off to be observable. Then F2 (forced hand-off) + Option C together
(they share `g_handoff_requested`). Re-run ON after F1 alone to confirm the deadlock clears
(`ge.frames` advances, `prims > 0`); then add F2 + C for the priority-inversion bound and the GE
serialization.

---

## 7. The four safety arguments (for the recommended fix: F1 + F2 + Option C)

**Deadlock-free.** F1 removes the AB-BA wait at the root: no thread ever holds an object mutex
(`s->mtx`/`ef->mtx`/`m->mtx`/render mtx) while blocked in `token_acquire`, so the
"has-mutex-wants-token ↔ has-token-wants-mutex" cycle cannot form. The producer's
`SignalSema`/`SetEventFlag`/drain always finds the object mutex free and can notify. F2's
`g_handoff_requested` only adds a void-release at a back-edge (the non-deadlocking choice, per
perf-design §C.R1); it never makes a holder keep the token when a peer needs it. The GE window
(Option C) is bounded because the GE callback is non-blocking (`sched-d-ge-callback-rca.md` §2) and
F2 forces the holder to free the token so the window opens in one budget window. No new lock; lock
order `object_mtx → g_sched_mutex` preserved and strengthened (§5.2).

**OFF byte-identical (OFF currently PASSES — unchanged).** Every new branch is gated:
`sched_token_release_for_wait`/`_reacquire_after_wait` no-op on `!token_enabled()`
(`psp_scheduler.cpp:493,508`); F1's un-nesting is a code-motion that, OFF, leaves the helpers as
no-ops so the object mutex is held across exactly the same `cv.wait_*` as today (the
`reacquire_after_wait` being a no-op, moving it outside the scope changes nothing OFF — the OFF poll
loop is byte-identical). The added re-check `while` loops (eventflag/mutex paths) only re-evaluate a
predicate that was already true on a real notify — OFF behavior is the original single
`cv.wait_*`-then-check. `g_handoff_requested` and `g_ge_intr_active` are touched only in
token_acquire/token_handoff/the GE bracket, all gated `!token_enabled() → return`, so OFF never sets
them. **Confirm against the OFF gate: `ge.frames=250, prims=3977`, byte-identical render verdicts.**

**Single-CPU ordering preserved (exactly-one-guest-runs).** F1 does not change *who* holds the
token, only *where* the blocking acquire sits — still exactly one `g_token_holder` runs guest code.
F2's forced hand-off transfers the token to exactly one selected peer (priority + RR via
`token_release_to`/`select_next_runnable`) before the holder continues — the holder either keeps
running (no peer needs it) or yields to exactly one peer. Option C's window guarantees no PSP thread
holds the token while the GE callback runs (the one-CPU interrupt). At no point do two guest streams
run; the R2 free-claim still admits exactly one claimant (perf-design §C.R2 proof unchanged).

**Generic (purity gate).** No game constants. F1 is a structural rule in the generic HLE/render core
(`psp_hle_kernel_sema.cpp`, `_eventflag.cpp`, `_mutex.cpp`, `_lwmutex.cpp`, `psp_hle_utility.cpp`,
`psp_render_queue.cpp`) and the generic scheduler. 271 / `FUN_0886207C` / CriCond uids are guest
facts observed live, never written into the fix. F2's `g_handoff_requested` and Option C's window are
title-agnostic. The fix helps any title whose cross-thread hand-off uses a guest mutex/eventflag
critical section (Patapon's sema-271 ring AND `.hack`'s CriCond ring) by the same generic mechanism.

---

## 8. Open concerns for review

1. **F1 re-check loops must cover every `break`/grant path.** The eventflag/sema-FIFO/mutex/lwmutex
   sites that do not already outer-loop need the dropped-`object_mtx` window re-validated after the
   token reacquire; a missed re-check could return SCE_OK on a stale predicate. Verify each wired
   site re-evaluates its guest condition under the re-taken object mutex (the WaitSemaCB poll loop
   already does; the others need the small `while`).
2. **Verify the deadlock clears with F1 ALONE before adding F2/C.** F1 is claimed as the root fix;
   the verifier must re-run Patapon ON after F1 and confirm `prims > 0` / `ge.frames` advancing and
   the sema-271 trace no longer ending at a free-but-unacquired count=1. If it does NOT clear with F1
   alone, the mechanism in §2 is incomplete and F2/C may be masking rather than fixing.
3. **`g_handoff_requested` granularity.** A single global flag forces a hand-off on *any* peer
   un-park, which may over-yield under many threads (Patapon's 8 / `.hack`'s 8+). If F2 causes
   thrashing, refine to a strictly-higher-priority gate (`g_higher_prio_waiting`, perf-design §D)
   instead of any-peer. Start with the simple flag; measure `g_valve_nullptr_claims` and hand-off
   frequency.
4. **`.hack` validation is mandatory, not inferred.** §3 argues `.hack` shares the root by code
   inspection; the claim "the fix unblocks `.hack`" must be proven on the `.hack` N≥30 freeze-rate
   harness (`build-hack/`, `PSPRECOMP_GAME=none`), not asserted. The eventflag path (F1) is the
   `.hack`-relevant site; confirm the CriCond producer's `SetEventFlag` no longer blocks on
   `ef->mtx`.
5. **TOCTOU on the dropped object mutex under high signal churn.** Between dropping `s->mtx` and the
   token reacquire, a peer could acquire-and-release 271 several times. The re-check loop handles
   correctness (re-validate under the mutex), but confirm no guest path relied on the WaitSemaCB
   "observe exactly the count at wake" timing (Patapon's FIFO-sema #29 handoff invariant).
6. **Interaction with the 6 audio threads (not spawned in the wedge).** Post-fix, the audio threads
   spawn (they did in OFF). Confirm the forced hand-off + un-nested re-acquire scale to 8 threads
   without `g_rr_cursor` starving a low-priority audio thread (within-priority RR must still rotate).

---

## 9. Structured summary

- **wedge_state:** Patapon ON steady wedge: `ge.frames=1, prims=0`; exactly 2 threads
  (`user_main` uid256/pri32, `FileThread` uid270/pri111) both `RUNNING` in `wait="semacb:271"`;
  `recent_funcs` head byte-identical across 30 s (frozen); sema 271 left **count=1 (FREE/unlocked)**
  at the wedge (last trace event `Signal count=0->1`); WaitSemaCB acquire predicate
  `count>=signal && waiters.empty()` is satisfiable but neither thread re-polls — run-token / object-
  mutex starvation, NOT a guest deadlock. IO state machine reached `new_state=300`; GE finish-cb
  fired once off-token (identical to OFF).
- **proven_mechanism:** hybrid **(c)+(a)**. Root = **(c)**: object mutex (`s->mtx`/`ef->mtx`) held
  across the *blocking* `sched_token_reacquire_after_wait()` (`token_acquire` parks while
  `s->mtx`/`ef->mtx` is still held — `sema.cpp:446-481`, `eventflag.cpp:189-203`), producing an AB-BA
  wait: A has-mutex-wants-token ↔ B has-token-wants-mutex. Amplifier = **(a)**: F3 + DelayThread
  keep-token-while-sleeping let B run/sleep holding the token so A reliably reaches the stranded-mutex
  state. **(b)** R2/`g_rr_cursor` explains the non-determinism/priority-blindness, not the hang.
  Evidence: free-but-unacquired sema, both threads in the poll loop, the un-nested-blocking-acquire
  code path, OFF (no-op helpers) not hitting it.
- **dothack_same_root:** `true`. Shared generic `psp_hle_kernel_eventflag.cpp` has the identical
  "blocking token re-acquire under `ef->mtx`" structure; `.hack`'s CriCond producer `SetEventFlag`
  takes `ef->mtx` and blocks on a token-starved consumer holding it — same AB-BA wait, same F3
  amplifier, same ~50% non-deterministic freeze signature.
- **recommended_fix:** Option 1 (un-nest: drop the object mutex BEFORE the blocking token re-acquire,
  re-take + re-check after) as the load-bearing deadlock fix, plus Option 2a (needed-peer forced
  hand-off via `g_handoff_requested`, honored at the holder's next back-edge/yield) to remove the F3
  amplifier and bound priority inversion. Option 3 (R2/RR) needs no rewrite once peers un-park
  cleanly; verify `g_valve_nullptr_claims` stays bounded.
- **fix_sketch:** see §5.1 (per-site F1 un-nesting rule + `g_handoff_requested` F2) and §5.2 (lock
  order). Invariant: *`sched_token_reacquire_after_wait()` is ALWAYS called with no object/render
  mutex held.*
- **f3_verdict:** **keep-constrained.** F3 is not the deadlock; keep its sole-runnable win, constrain
  it with the needed-peer forced hand-off so it cannot starve a peer the holder must yield to.
- **safety:** deadlock — AB-BA cycle removed at the root (no object mutex held across a blocking token
  wait), F2 only void-releases (non-deadlocking), GE window bounded by a non-blocking callback;
  off_identical — all paths gated by `sched_token_enabled()`, helpers no-op OFF, F1 is OFF-invisible
  code motion, flags never set OFF, confirm `ge.frames=250/prims=3977`; single_cpu — token holder
  count unchanged, forced hand-off transfers to exactly one peer, R2 admits one claimant, GE window
  excludes all PSP threads; generic — structural rule in generic HLE/scheduler/render core, no game
  constants, helps both Patapon (sema-271) and `.hack` (CriCond) identically.
- **hazard2_integration:** Option C's GE-interrupt-window enter sets the SAME `g_handoff_requested`,
  so a sole-runnable F3 holder is forced to free the token at its next back-edge and the GE window
  opens in bounded time (resolves Option C open-concern #2). Land F1 first (precondition), then F2 +
  Option C together (shared flag).
- **open_concerns_for_review:** §8 (re-check-loop completeness; verify F1-alone clears the deadlock;
  forced-hand-off granularity; mandatory `.hack` N≥30 validation; dropped-mutex TOCTOU; 8-thread RR
  scaling).
- **doc_path:** `.planning/research/sched-d-hazard1-rca.md`

---

## 10. F1 implementation notes (un-nesting only — F2/Option C NOT in this increment)

Implemented Option 1 (F1) ONLY: the blocking token re-acquire is un-nested from under every object/
render mutex. F2 (needed-peer forced hand-off / `g_handoff_requested`) and Hazard 2 Option C (GE
interrupt window) are explicitly NOT done here — they are the separate next increment.

### 10.1 The mechanism: one new helper, applied uniformly
New generic scheduler helper `sched_token_reacquire_unlocked(std::unique_lock<std::mutex>& lock)`
(`runtime/src/psp_scheduler.cpp`, declared in `runtime/include/psp_scheduler.h`). On cv-wake the
object mutex is re-locked by `std::condition_variable`; this helper then does
`lock.unlock()` → `token_acquire(g_sched_mutex, g_current)` (blocking re-acquire with NO object mutex
held) → `lock.lock()`. The caller re-checks its wait predicate after it returns. It is a **literal
no-op OFF** (`!token_enabled()` returns before touching `lock`), so OFF leaves the object mutex held
across exactly the original `cv.wait_*` with zero extra unlock/lock ops. This is the established
invariant: **`sched_token_reacquire_after_wait()`/the blocking token re-acquire is ALWAYS called with
NO object/render mutex held.**

### 10.2 Sites fixed (every wired object-cv / token-reacquire wait) + per-site OFF-identity
- **sema deleted-path** (`psp_hle_kernel_sema.cpp`, ~:280-296): replaced bare reacquire with
  `sched_token_reacquire_unlocked(lock)` + re-check `sema_ready()` after the dropped-`s->mtx` window.
  OFF: helper no-op ⇒ `s->mtx` held continuously across the single 100ms `cv.wait_for`; the re-check
  `sema_ready()` re-evaluates the same predicate under the same continuously-held lock ⇒ same `got_it`.
- **sema slow timed FIFO** (~:353-364): un-nested the timeout-bail reacquire. The waiter is removed
  from the queue (`sema_remove_waiter`) BEFORE the un-nest, so dropping `s->mtx` cannot dangle the
  queue pointer; the TIMEOUT result is final. OFF: no-op.
- **sema slow unbounded FIFO** (~:374-394): un-nested both the shutdown-bail reacquire and the GRANTED
  success reacquire. On the granted path the signaller already popped this waiter and transferred the
  count (`waiter.granted` monotonic-true) ⇒ no re-check needed; dropping `s->mtx` is safe. OFF: no-op
  (helper leaves `s->mtx` held across exactly the original FIFO loop).
- **sema WaitSemaCB poll** (~:500): moved the reacquire to the unlocked helper inside the poll-loop
  block; the EXISTING outer `while` re-checks `acquire_ok()` under `s->mtx` at the top (TOCTOU
  covered). OFF: the 5ms poll cadence and `s->mtx`-held-throughout behavior are byte-identical.
- **eventflag WaitEventFlag** (`psp_hle_kernel_eventflag.cpp`, ~:194-224): wrapped the park in a
  `while (!g_should_exit)` loop; park on `ef->cv` under `ef->mtx`, then
  `sched_token_reacquire_unlocked(lock)`, then re-evaluate the pattern (`evf_ready()`); ON re-parks if
  unmatched, OFF breaks after one wait. OFF: helper no-op ⇒ `ef->mtx` held across exactly the original
  single `cv.wait_for`; the loop runs once and returns the same `matched` (byte-identical). This is
  the `.hack` CriCond-relevant site.
- **mutex LockMutex** (`psp_hle_kernel_mutex.cpp`, ~:99-114): park loop under `m->mtx`, then
  `sched_token_reacquire_unlocked(lock)`, then an ON-only re-park loop re-checking `acquired()` (owner
  may flip while `m->mtx` is dropped). OFF: first `while` is the original `cv.wait_for` loop, helper
  no-op, second `while` gated `sched_token_enabled()` never runs ⇒ byte-identical.
- **lwmutex lw_lock_blocking** (`psp_hle_kernel_lwmutex.cpp`, ~:211): the single post-`for(;;)`
  reacquire is now `sched_token_reacquire_unlocked(lock)`. Every break is final under `g_lw_mtx`; on
  the success break `lw_fast_lock` already committed ownership to the guest workarea, so a peer cannot
  steal the lock while `g_lw_mtx` is dropped (no re-check needed). OFF: no-op. `.hack`-relevant.
- **render_queue** (`psp_render_queue.cpp`): `render_queue_post` (~:73, `g_render_req.done` monotonic,
  return immediately) and `render_queue_draw_sync` mode-0 (~:162, `drained` only logged) both use the
  unlocked helper under `g_render_mutex`. OFF: no-op; on a non-PSP thread (`g_current==null`) also a
  no-op. Removes the render-thread `render_queue_process` blocking on a PSP thread that stranded
  `g_render_mutex` in `token_acquire`.
- **SendMsgPipe** (`psp_hle_utility.cpp`, ~:1739-1747): ALREADY F1-compliant by construction — the
  `std::lock_guard<std::mutex>` on `g_msgpipe_mtx` is scoped to the inner `{ }` that closes BEFORE the
  `release_for_wait()/sleep_for/reacquire_after_wait()`, so the blocking reacquire already runs with
  no object mutex held (the park is a host `sleep_for`, not a cv-wait under the object mutex). Comment
  added; no functional change.

Lock order `object_mtx -> g_sched_mutex` preserved/strengthened: F1 REMOVES the hold-object-mutex-
block-on-token path and adds NO reverse (the only place both are touched is the transient non-blocking
`release_for_wait`, which always releases `g_sched_mutex` before the object-cv park). Generic only —
no game constants. F2/DelayThread/Option C untouched.

### 10.3 Build + smoke results
- `cargo test -q`: 299 passed, 0 failed (no Rust change — runtime C++ only).
- Build: `runtime/build` (Patapon, PSPRECOMP_GAME=patapon) and `build-hack` (`.hack`,
  PSPRECOMP_GAME=none) both link cleanly.
- **Patapon flag-OFF ~20s:** HEALTHY. `ge.frames=473, prims=6726, real_nonsprite=6254`, GE heartbeat
  firing, all 8 threads (incl. the 6 `sgx-psp-*` audio) spawned, no crash. OFF unaffected by F1.
- **Patapon flag-ON ~77s (PSPRECOMP_PREEMPT=1 PSPRECOMP_CROSS_MID=1) — THE GATE: PASS.** The GE
  livelock CLEARED. `ge.frames` advanced 1 → 310 (@38s) → 637 (@77s), `prims` 0 → 309 → 636 (steadily
  climbing, ~10 fps), DRAW_PRIM reached #625, GE_GEOM_HEARTBEAT started (2 beats). **The 271 WaitSemaCB
  wedge is GONE:** no thread is stuck in `wait="semacb:271"`; all 8 threads spawned and RUNNING/
  progressing (the wedge had only 2 threads, both frozen in `semacb:271` with a byte-identical frozen
  `recent_funcs` head). `recent_funcs` advances across snapshots. The diagnosis's mandatory F1-alone
  gate (§8 #2: deadlock clears with F1 alone, no F2/C) is satisfied. (`real_nonsprite` ramp and full
  N-boot acceptance are the separate verifier's job; this increment establishes "ON renders now.")
