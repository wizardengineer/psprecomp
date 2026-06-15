# sched-d F2 + Option C — needed-peer forced hand-off + GE-interrupt window: implementation spec

**Status:** authoritative, implementation-ordered design on `feat/single-runnable @ b5187ce` (F1 landed,
verified). Source-of-truth pass: every code reference below is read from the COMMITTED tree at b5187ce.
This spec resolves the **token-starvation livelock** that the F1 verifier proved is the residual
limiter (`.planning/research/sched-d-f1-verify-result.md`): F1 removed the hard AB-BA mutex-strand
deadlock but a sole-runnable holder still **F3-keeps / re-claims the token** while the
producer/consumer that must advance the ring is starved. F2 makes rotation FAIR; Option C lands with
it via the shared `g_handoff_requested` flag.

**Prerequisite (already true at b5187ce):** F1 — `sched_token_reacquire_unlocked()` un-nests the
blocking re-acquire from under every object/render mutex (`runtime/src/psp_scheduler.cpp:533-547`).
F2 builds on top; it does NOT re-touch the F1 un-nesting.

**Headline:** the limiter is `token_handoff`'s F3 sole-runnable short-circuit
(`runtime/src/psp_scheduler.cpp:208` `if (!other_runnable_exists(self)) return;`) combined with two
fairness gaps: (1) when the starved peer un-parks it is invisible to the holder until the holder
happens to reach a back-edge AND a peer is already eligible — but the un-parking peer sets nothing,
so a tight clear/flip loop or a `SetEventFlag`-in-a-loop producer keeps short-circuiting; (2) even
when a hand-off fires, the just-ran holder re-contends immediately and the R2 first-free claim
(`:152`, `:176-179`) plus the `g_rr_cursor`-anchored scan can hand the token straight back, so ONE
forced hand-off does not converge. F2 fixes both: a peer that becomes runnable **requests** a hand-off
(`g_handoff_requested`), the holder **must** honor it at its next back-edge (skipping F3), AND the
just-ran holder is made **not immediately re-selectable** ahead of the peer that was waiting
(fairness fix — REQUIRED, §3.3).

---

## 0. The confirmed mechanism (what F2 must defeat)

From `sched-d-f1-verify-result.md` (GATE B + GATE C-FAST), reproduced live at b5187ce:

- **.hack flag-ON: 100% frozen at 2 frames.** All 10 threads `RUNNING wait=''` (spinning, NOT parked).
  56 `SetEventFlag(CriCond uid 265/268/271)` producer fires + 62 `WaitEventFlag` consumer, zero
  forward progress. The CriCond ring producer monopolizes the token: each `SetEventFlag`
  (`psp_hle_kernel_eventflag.cpp:126-128`, takes only `ef->mtx`, ORs the pattern, `notify_all`,
  returns) runs back-to-back as the sole runnable while the woken consumers cannot get token time to
  drain and advance the ring.
- **Patapon flag-ON: `ge.frames` ramps ~10 fps but `real_nonsprite` STUCK at 0 for 180 s.** Every
  `DRAW_PRIM` is `clear=1`; ZERO `clear=0` real-geometry draws. The token-holder runs the clear/flip
  loop to its back-edge while the geometry-producer thread (gated behind the sema-271 IO ring
  hand-off) never gets enough token time to emit real draws. Audio producers also starved.

The common shape: **a holder keeps or instantly re-claims the token; the thread that must advance the
ring / submit real draws is token-starved and never makes forward progress.** F1 turned a
non-deterministic mutex deadlock into a deterministic token-starvation livelock. F2 is the fairness
fix.

---

## 1. The exact limiter in code (b5187ce)

### 1.1 F3 short-circuit — the keep path
`token_handoff` (`runtime/src/psp_scheduler.cpp:198-216`):
```cpp
static void token_handoff(std::unique_lock<std::mutex>& lock, PspThread* self) {
    if (!token_enabled() || g_token_holder != self) {
        return;
    }
    if (!other_runnable_exists(self)) {   // :208  F3 sole-runnable keep
        return;
    }
    token_release_to(self);
    if (g_token_holder == self) { return; }
    token_acquire(lock, self);
}
```
F3 keeps the token when no peer is *currently* eligible. The starved peer is `token_parked` (parked on
a foreign object cv after `sched_token_release_for_wait()`, `:492-505`) right up until a signal wakes
it; the instant it wakes it re-enters `token_acquire` and clears `token_parked` (M2, `:143`), but by
then the holder may already be deep in a syscall-free clear/flip loop that only reaches `token_handoff`
via `sched_preempt` back-edges every `SCHED_PREEMPT_BUDGET` (20000) passes — and even at that back-edge,
nothing tells the holder "a peer needs you NOW."

### 1.2 R2 first-free re-claim — the instant re-monopolize
`token_acquire` (`:152-155`, `:176-179`): a freed token (`g_token_holder == nullptr`) is claimed by
whoever runs the predicate first. After a forced hand-off, the just-ran holder loops back to its next
syscall almost immediately; if the peer has not yet re-parked-then-woken-then-re-contended, the holder
re-takes the free token. This is the "monopolizer re-claims it" race the prompt flags.

### 1.3 `g_rr_cursor` anchoring — the directed-grant gap
`select_next_runnable` (`:76-104`) scans from `g_rr_cursor + 1` and picks lowest-priority-number. But
`g_rr_cursor` is set to the index of the **last thread granted** (`token_release_to`, `:118-124`).
After a hand-off A→B, `g_rr_cursor` points at B; the next scan starts after B — good for B→others, but
within a priority the just-ran thread can still win on the next round because nothing records "A just
ran, prefer a peer over A." Without a fairness rule the directed grant can ping-pong A↔B (or, worse,
A monopolizes because B re-parks before A's next handoff sees it).

---

## 2. F2 — needed-peer forced hand-off (core fix)

### 2.1 The flag
Add to `psp_scheduler.cpp` (file-static, guarded by `g_sched_mutex`, ON-only):
```cpp
/// [F2] A peer became runnable and needs token time; the current holder MUST
/// hand off at its next back-edge/yield, skipping the F3 sole-runnable keep.
/// Guarded by g_sched_mutex. Set ON-only (token_enabled()); never touched OFF.
static bool g_handoff_requested = false;
```
This is the single shared mechanism F2 and Option C both use (§4). It is dead OFF: every site that
sets or reads it is inside an `if (token_enabled())` / `if (!token_enabled()) return;` gate, so OFF
the flag stays `false` forever and no OFF branch observes it.

### 2.2 WHO SETS IT and WHEN — the concrete generic rule

> **Rule (generic, no game constants):** Whenever a thread transitions from
> token-parked/blocked to *re-contending for the token while a DIFFERENT thread is the holder*, it
> sets `g_handoff_requested = true`. This is exactly the "a peer just became runnable while the
> holder would otherwise monopolize" event, expressed without any per-game knowledge.

The single set-site is inside `token_acquire`, immediately after M2 clears `token_parked`
(`runtime/src/psp_scheduler.cpp:143`) and after the already-holder guard (`:148-150`), at the point
where the function determines the token is held by someone else:
```cpp
self->token_parked = false;                       // :143  (M2, existing)
if (g_token_holder == self) { return; }           // :148  (existing guard)
if (g_token_holder == nullptr) {                  // :152  (existing first-free)
    g_token_holder = self;
    return;
}
// [F2] Token is held by a peer and THIS thread is now re-contending (it just
// un-parked / was resumed / was signalled and re-entered here). It needs token
// time; request the holder hand off at its next back-edge. Set ON-only.
g_handoff_requested = true;                        // NEW
while (g_token_holder != self && !g_should_exit.load()) { ... }  // :164 (existing)
```

**Why this set-site is correct and covers the confirmed mechanism:**
- Every path by which "a peer becomes runnable" routes through `token_acquire` with a foreign holder:
  - **SetEventFlag / SignalSema wake** → the consumer's `ef->cv` / `s->cv` predicate fires →
    `sched_token_reacquire_unlocked` → `token_acquire` (`:544`). The woken consumer sets the flag.
    This is the `.hack` CriCond case and the Patapon sema-271 case directly.
  - **resume** (`psp_thread_resume`, `:846-860`) sets READY + notifies; that thread's next guest
    re-entry takes the token via `token_acquire`.
  - **sleep/wait-end tails** (`psp_thread_sleep_current:723`, `psp_thread_wait_end:814`) call
    `token_acquire` on wake — same set-site.
- The producer firing `SetEventFlag` in a loop is the holder; the consumer it wakes is the one that
  re-enters `token_acquire` and sets `g_handoff_requested`. So the very act of waking the starved
  consumer arms the forced hand-off against the monopolizing producer. This is precisely the
  `.hack` ring: producer monopolizes → its `notify_all` wakes a consumer → consumer arms the flag →
  producer's next back-edge MUST yield → consumer drains and advances the ring.

**Why NOT set it from the signaller (SetEventFlag/SignalSema) directly:** the signaller does not hold
`g_sched_mutex` (it holds `ef->mtx`/`s->mtx`), and setting a `g_sched_mutex`-guarded flag from there
would either nest `ef->mtx → g_sched_mutex` (lock-order inversion the design forbids) or need a second
lock acquire. Routing the set through the woken consumer's `token_acquire` re-entry keeps the flag
strictly under `g_sched_mutex` with no new lock and no inversion. The woken consumer reliably reaches
`token_acquire` because F1 un-nested the re-acquire (it always runs, even on a spurious wake, because
the consumer re-checks its predicate after; if the predicate is still false ON it re-parks via
`sched_token_release_for_wait` and the cycle is harmless).

### 2.3 Quantum-elapsed fallback trigger (defense-in-depth)
The set-site above arms the flag on a peer un-park. As a second, independent trigger — covering a peer
that is READY but not currently parked-then-woken (e.g. just-started, or a busy peer that has not yet
hit its own `token_acquire`) — also arm the flag from the **budget back-edge** when a holder has run a
full quantum AND a peer is eligible. In `token_handoff`, before the F3 check:
```cpp
static void token_handoff(std::unique_lock<std::mutex>& lock, PspThread* self) {
    if (!token_enabled() || g_token_holder != self) { return; }
    // [F2] Honor a pending forced hand-off OR a genuinely eligible peer; only
    // F3-keep when NEITHER a hand-off is requested NOR any peer is runnable.
    if (!g_handoff_requested && !other_runnable_exists(self)) {
        return;                                   // F3 keep — now needed-peer aware
    }
    g_handoff_requested = false;                  // consume the request
    token_release_to(self);
    if (g_token_holder == self) { return; }       // kept (no runnable peer)
    token_acquire(lock, self);
}
```
This keeps F3's speed win (when there is genuinely no peer, both conditions are false → keep, zero cv
ops) and forces a hand-off the moment a peer is eligible OR has explicitly requested one. The
`sched_preempt` budget (`SCHED_PREEMPT_BUDGET = 20000`, `psp_scheduler.h:63`) is the "you have held
long enough" quantum: a syscall-free holder reaches `token_handoff` every ~20k back-edges and, if any
peer is now eligible, must yield. The set-site (§2.2) is the fast trigger (a signalled peer arms it
immediately); the back-edge `other_runnable_exists` is the slow safety trigger.

### 2.4 WHERE it is honored
- **`token_handoff` (`:198`, the F3 site)** — shown in §2.3. This is the primary honor point: the
  unified hand-off used by `sched_yield_point` (`:384`), and thus by `sched_preempt` (`:480`) and the
  DelayThread/SleepThread/WaitThreadEnd leading yields.
- The R1 void-release park sites (`sched_token_release_for_wait:492`, `psp_thread_sleep_current:688`,
  `psp_thread_wait_end:781`, `psp_thread_exit_current:565`) already free the token unconditionally —
  they do NOT F3-keep, so they need no change for F2 (a holder that parks always releases). F2 only
  has to defeat the *keep* path, which is exclusively `token_handoff`.

---

## 3. Fairness — the part ONE forced hand-off does NOT solve

### 3.1 The residual after a naive F2
Force ONE hand-off A→B. B drains, advances the ring, then B reaches its own next syscall/back-edge and
calls `token_handoff`. If A is once again the only eligible peer (B just re-parked, or A re-contends
faster), the token goes back to A. If A then re-monopolizes (clear/flip loop, or `SetEventFlag` loop)
before B re-contends, we are back to starvation. The prompt sharpened exactly this: *"forcing ONE
hand-off is not enough if the starved thread gets the token then immediately returns it or the
monopolizer re-claims it."*

### 3.2 Two fairness defects to close
1. **R2 first-free re-claim (`token_acquire:152`, `:176-179`):** after a forced void-release the
   just-ran holder can re-claim the free token before the intended grantee runs. Today `token_handoff`
   uses `token_release_to` (a DIRECTED grant via `select_next_runnable`), not a void-release, so the
   token goes straight to a selected peer — GOOD, the R2 path is not hit on the F2 hand-off itself.
   But the R1 park sites and `sched_token_release_for_wait` DO void-release, and a peer woken there
   races the holder for the free token. The directed-grant `token_handoff` is the F2 path; the R2
   race is a latent backstop, monitored via `g_valve_nullptr_claims` (`:57`).
2. **`g_rr_cursor` does not exclude the just-ran holder (`select_next_runnable:76-104`):** the scan
   picks lowest-priority-number from `g_rr_cursor+1`. For EQUAL priority it rotates correctly (cursor
   advances past the granted thread). But the just-ran holder is a candidate again on the very next
   selection, so a holder that hands off and immediately re-contends can be re-picked ahead of a
   peer that was waiting longer — within-priority ping-pong, and cross-priority the higher-priority
   producer always re-wins (priority inversion against a lower-priority consumer that must drain).

### 3.3 Fairness verdict — REQUIRED (yes), specifically a "just-ran" demotion

> **A `g_rr_cursor`/`select_next_runnable` fairness fix IS required in addition to F2.** F2 forces
> the hand-off; without fairness the token can ping-pong straight back to the just-ran holder and the
> consumer still starves. The minimal, generic fix: make the just-handed-off thread **not preferred
> over a peer that is already contending** on the next selection.

**Concrete fairness mechanism (generic, no game constants):** add a per-thread `last_ran_token`
ordering so `select_next_runnable` breaks ties (and resists immediate re-selection of the just-ran
holder) by "least-recently-ran among equal priority," and have `token_handoff` pass the *current*
holder as the `exclude` argument (it already does — `token_release_to(self)` → `select_next_runnable(self)`
excludes `self`). The directed grant already excludes the just-ran holder for the duration of the
hand-off; the remaining gap is the *immediate re-contend*. Close it with a one-shot:

```cpp
// In token_handoff, after a successful directed grant to a peer:
//   record that `self` just yielded, so on `self`'s immediate re-contend the
//   peer (now holder) is not preempted by `self` via the budget back-edge.
```

The simplest sufficient form (recommended starting point): in `token_acquire`, when `self`
re-contends and a peer holds the token, `self` parks (it does NOT R2-claim unless the token is
genuinely free) — which is already the behavior (`:164` while-loop parks until granted). The ping-pong
risk is therefore concentrated in the *budget back-edge re-arming* (§2.3): if A hands to B, then A's
next back-edge sees B eligible and `g_handoff_requested` could be re-armed by B's own re-contend,
forcing B→A again. **Guard against immediate reversal:** after `token_handoff` performs a directed
grant, do NOT let the same holder re-arm a forced hand-off for at least one of the new holder's
quanta. Implement with a generation/round counter:

```cpp
static uint64_t g_handoff_generation = 0;   // bumped on every directed grant (g_sched_mutex)
// per-thread: uint64_t last_handoff_gen = 0;
// In token_handoff before re-arming F3/budget hand-off:
//   only honor g_handoff_requested if it was set in a LATER generation than the
//   one in which `self` last yielded — i.e. a NEW peer event, not self's own echo.
```

**Recommendation:** ship F2 (§2) FIRST with the within-priority RR already present
(`select_next_runnable` rotates via `g_rr_cursor`), measure. If GATE B/C show ping-pong (high
`g_valve_nullptr_claims` or `real_nonsprite` still 0 / `.hack` still frozen), add the
`g_handoff_generation` guard. The verifier proved F2-absent starves; the open question is whether
plain directed-grant + existing RR converges or needs the generation guard. **Design both; gate the
generation guard behind the same flag; land F2 + the guard together if the first GATE pass shows
reversal.** Given the confirmed monopolization is *deterministic* (.hack 100% frozen), the safe
default is to **include the generation guard from the start** — it is generic, OFF-dead, and removes
the only remaining ping-pong path.

### 3.4 Convergence argument
- **.hack CriCond:** producer P holds token, fires `SetEventFlag`, `notify_all` wakes consumer C. C's
  `WaitEventFlag` predicate matches → `sched_token_reacquire_unlocked` → `token_acquire`: C clears
  `token_parked`, sees P is holder, sets `g_handoff_requested = true` (generation N). P's next
  back-edge (`sched_preempt`/its next syscall) hits `token_handoff`: `g_handoff_requested` is set →
  P does `token_release_to(P)` → `select_next_runnable(P)` excludes P → grants C → `g_handoff_generation`
  bumps to N+1, C is holder. C drains the ring, advances qhead/qtail, advances `recent_funcs`. When P
  re-contends, P parks (token held by C); P cannot re-arm a hand-off in generation N (its own echo) —
  only a genuinely later peer event re-arms. C makes forward progress. Freeze clears.
- **Patapon geometry:** the clear/flip loop holder is forced to yield to the geometry-producer the
  moment the producer un-parks (its IO-ring sema-271 wait completes and it re-contends, arming the
  flag). The producer gets token time, runs the path that builds real draw commands, emits `clear=0`
  draws. `real_nonsprite` ramps off 0.

---

## 4. Option C — GE-interrupt window (Hazard 2), lands WITH F2 via the shared flag

### 4.1 The two new primitives
Add to `psp_scheduler.{h,cpp}`, ON-only, callable from the main/render thread (`g_current == null`) —
these intentionally do NOT early-return on `!g_current`; they early-return ONLY on `!token_enabled()`:
```cpp
static bool g_ge_intr_active = false;          // guarded by g_sched_mutex (ON-only)
static std::condition_variable g_ge_intr_cv;   // main thread waits for token free

void sched_ge_interrupt_enter() {
    if (!token_enabled()) { return; }                       // OFF: byte-identical no-op
    std::unique_lock<std::mutex> lock(g_sched_mutex);
    g_handoff_requested = true;     // [F2 shared] force a sole-runnable F3 holder
                                    // to release at its next back-edge — THIS is
                                    // what bounds the enter-wait.
    g_ge_intr_cv.wait(lock, [] {
        return g_token_holder == nullptr || g_should_exit.load();
    });
    g_ge_intr_active = true;        // block any PSP thread from taking the free token
    // DROP g_sched_mutex before returning: the callback runs guest code and takes
    // ef->mtx (SetEventFlag). Holding g_sched_mutex across guest code would invert
    // object_mtx -> g_sched_mutex. g_ge_intr_active (re-checked under g_sched_mutex
    // by token_acquire) is what actually excludes PSP threads.
}

void sched_ge_interrupt_leave() {
    if (!token_enabled()) { return; }
    std::unique_lock<std::mutex> lock(g_sched_mutex);
    g_ge_intr_active = false;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (g_threads[i].in_use) { g_threads[i].cv.notify_one(); }
    }
}
```

### 4.2 The `token_acquire` exclusion (one ON-only condition)
A PSP thread may not take the free token while `g_ge_intr_active`. Edit `token_acquire`
(`runtime/src/psp_scheduler.cpp`):
- First-free fast path (`:152`): `if (g_token_holder == nullptr && !g_ge_intr_active)` — and when it
  DOES take the free token, that is also the moment to notify `g_ge_intr_cv` if it transitions to/from
  free. Simpler: the enter-wait predicate is `g_token_holder == nullptr`; a PSP thread taking the
  token sets `g_token_holder = self`, so the main thread's wait will not be satisfied. The exclusion
  prevents a PSP thread from taking it during the window.
- R2 predicate (`:168-172`) and the in-body claim (`:176`): add `&& !g_ge_intr_active`; the predicate
  also wakes on `!g_ge_intr_active`.
- When a PSP thread void-releases the token (`token_release_to` finds no peer, or the R1 sites set
  `g_token_holder = nullptr`), it must `g_ge_intr_cv.notify_one()` so a waiting `enter` sees the free
  token. Add a `g_ge_intr_cv.notify_one()` at each void-release point under the flag
  (`sched_token_release_for_wait:502`, `psp_thread_sleep_current:689`, `psp_thread_wait_end:782`,
  `psp_thread_exit_current:566`, and in `token_release_to` when `next == nullptr` keep path — though
  there the holder keeps it, so no notify). Cleanest: notify whenever `g_token_holder` is set to
  `nullptr` under `g_sched_mutex`.

This whole edit is dead OFF (gated by `token_enabled()` at the top of `token_acquire:132` — the new
`&& !g_ge_intr_active` conditions are only reachable ON, and `g_ge_intr_active` is never set OFF).

### 4.3 Bracket ONLY the two `fn(...)` calls
In `runtime/src/psp_ge.cpp`, wrap exactly the guest-code dispatch:
- **FINISH** (`psp_ge.cpp:237` `fn(rdram, &fin_ctx);`):
  ```cpp
  sched_ge_interrupt_enter();
  fn(rdram, &fin_ctx);
  sched_ge_interrupt_leave();
  ```
- **SIGNAL** (`psp_ge.cpp:338` `fn(g_signal_rdram ? g_signal_rdram : rdram, &sig_ctx);`): same bracket.

No game constants: `g_finish_fn`/`g_signal_fn` are the guest's own registered pointers
(`ge_set_finish_callback:141`), never literals in the fix.

### 4.4 Why the enter-wait is bounded (the F2 coupling)
`sched_ge_interrupt_enter` waits for `g_token_holder == nullptr`. Before F2 this could hang forever if
a PSP thread F3-keeps the token sole-runnable (`sched-d-ge-callback-rca.md` open-concern #2). With F2:
`enter` sets `g_handoff_requested = true`; the holder's next `token_handoff` back-edge now sees the
flag set → MUST `token_release_to` → and since the GE window wants the CPU, the holder either grants a
PSP peer (then `enter` re-waits, but `g_handoff_requested` stays armed each cycle) or, if the GE
exclusion is honored before the grant... — to guarantee progress, the cleanest bound is: while
`g_ge_intr_active`-pending, `token_handoff`'s directed grant is suppressed in favor of a void-release
so `g_token_holder` reaches `nullptr` and the main thread's `enter` wins. Concretely, gate
`token_release_to` in the handoff path: if a GE interrupt is pending (a separate
`g_ge_intr_pending` set by `enter` before the wait), the holder void-releases
(`g_token_holder = nullptr; g_ge_intr_cv.notify_one();`) instead of granting a peer, so the window
opens in ONE `SCHED_PREEMPT_BUDGET` window (the holder's next back-edge). After `leave`, normal
directed grants resume. This makes the enter-wait bounded by exactly one quantum, and the callback
itself is non-blocking (`sched-d-ge-callback-rca.md` §2: writes ring, `SetEventFlag` under `ef->mtx`,
returns — never parks), so `leave` always runs.

**Set-of-flags summary (ON-only, all under `g_sched_mutex`):**
- `g_handoff_requested` — F2: a peer (or the GE window) needs a hand-off; honored in `token_handoff`.
- `g_ge_intr_pending` — Option C: a GE interrupt is waiting to enter; makes the holder VOID-release
  (not directed-grant) so `g_token_holder` reaches `nullptr`.
- `g_ge_intr_active` — Option C: the GE callback is running as sole guest executor; blocks PSP token
  acquisition.

---

## 5. Spin paths that could bypass the forced hand-off

| Spin path | Reaches `token_handoff`? | Covered by F2? | Handling |
|---|---|---|---|
| **WaitSemaCB poll loop** (`psp_hle_kernel_sema.cpp:453-507`) | Indirectly | YES | Each poll iteration calls `psp_kernel_check_callbacks` then, if not acquired, `sched_token_release_for_wait` (`:484`) → parks on `s->cv` → `sched_token_reacquire_unlocked` (`:500`) → `token_acquire`. The poll thread is `token_parked` during the wait (invisible, correct) and on each re-acquire it sets `g_handoff_requested` if a peer holds the token. The poll thread is NOT a monopolizer — it void-releases every iteration. The monopolizer is the *producer* it waits on; F2 arms the flag against that producer when this poll thread wakes. **Covered.** |
| **Tight guest loop region** (syscall-free, e.g. Patapon `[LISTLOOP]`, the clear/flip loop) | YES, via `sched_preempt` back-edges | YES | The emitter inserts `sched_preempt` at loop back-edges; once `ctx->preempt_budget <= 0` it calls `sched_preempt` (`psp_scheduler.cpp:469-481`) → `sched_yield_point` → `token_handoff`. With F2, if `g_handoff_requested` is set (a peer un-parked) the holder MUST yield. **Covered, bounded by `SCHED_PREEMPT_BUDGET` (20000).** Caveat: relies on the emitter actually inserting back-edge preemption points in the spin region — an emitter coverage gap would leave a truly syscall-AND-back-edge-free loop uncovered. That is an emitter concern (out of scope for F2), surfaced as open-concern §7.1. |
| **DelayThread holding the token across a real-time `sleep_for`** (`psp_hle_kernel_thread.cpp:305-307`) | Partially | **NO — needs separate handling** | `hle_sceKernelDelayThread` calls `psp_kernel_check_callbacks` → `sched_yield_point` (→ `token_handoff`, F2-honored here, GOOD) → **then `std::this_thread::sleep_for(usec)` while STILL HOLDING the token if F3 kept it.** Today the `sched_yield_point` at `:306` would F3-keep if no peer is eligible at that instant, then sleep holding the token. If a peer un-parks *during* the host sleep, F2's flag is set but the holder is asleep and will not re-check until after the sleep. **Fix: DelayThread must RELEASE the token across the host sleep** (it is a real-time sleep, not a guest-execution interval — the holder is not running guest code during `sleep_for`). Wrap the sleep: `sched_token_release_for_wait(); std::this_thread::sleep_for(...); sched_token_reacquire_after_wait();` — but per F1, the re-acquire must run with no object mutex held (DelayThread holds none, so a bare `sched_token_reacquire_after_wait()` is safe here, OR use the existing pattern). This makes the sleeping thread `token_parked` (invisible) for the sleep duration so a peer can run. **ON-only; OFF the helpers are no-ops so `sleep_for` behaves exactly as today (byte-identical).** This is the DelayThread keep-token amplifier the Hazard-1 RCA §2.4 named. |
| **SleepThread / WaitThreadEnd** (`psp_hle_kernel_thread.cpp:321`, `:516`) | YES | YES | These call `sched_yield_point` then `psp_thread_sleep_current` / `psp_thread_wait_end`, which void-release the token before parking (`:688`, `:781`) and re-acquire on wake (`:723`, `:814`). They do NOT keep the token across a host sleep. **Covered.** |
| **`render_queue_draw_sync` / `render_queue_post`** (`psp_render_queue.cpp:60-73`, `:150-162`) | Via leading `sched_yield_point` + release-for-wait | YES | Already F1-compliant: `sched_token_release_for_wait` before the cv wait, `sched_token_reacquire_unlocked` after. The thread is `token_parked` for the GE round-trip. **Covered.** |

**Net:** F2 (the flag + honor in `token_handoff`) covers the WaitSemaCB poll, the tight guest loop
(modulo emitter back-edge coverage), SleepThread/WaitThreadEnd, and the render round-trips. **The
DelayThread real-time `sleep_for` needs the separate token-release-across-host-sleep change** (§5 row
3) — it is the one path where the token is held across a non-guest-execution interval and F2's flag
cannot be observed until the sleep ends.

---

## 6. The four safety arguments (per mechanism)

### 6.1 F2 (needed-peer forced hand-off + fairness guard)
- **Deadlock-free.** F2 only ever causes a holder to **void-release / directed-grant** the token at a
  back-edge — the non-deadlocking choice (a holder that yields never blocks while holding). It never
  makes a holder keep the token when a peer needs it (the opposite of deadlock). No new lock:
  `g_handoff_requested` / `g_handoff_generation` are plain bools/counters under the existing
  `g_sched_mutex`. Lock order `object_mtx → g_sched_mutex` is unchanged (the flag is set inside
  `token_acquire`, which already holds only `g_sched_mutex`, with NO object mutex held per F1). The
  fairness guard only changes *which* peer is selected, never introduces a wait.
- **OFF byte-identical.** `g_handoff_requested`, `g_handoff_generation`, and the per-thread
  `last_handoff_gen` are touched ONLY inside `token_acquire` / `token_handoff`, both of which
  early-return at `if (!token_enabled()) return;` (`:132`, `:199`) before reaching any F2 code. OFF the
  flag stays `false`, the generation never bumps, `token_handoff` is never even called (the OFF
  `sched_yield_point` path at `:388-434` is taken instead). The DelayThread sleep-release change is
  gated: `sched_token_release_for_wait`/`_reacquire_after_wait` are no-ops OFF
  (`:493`, `:508`), so `sleep_for` runs exactly as today. **Gate: Patapon OFF CLEANROOM band
  (`real_nonsprite` 13.8k–15.7k, visible 100/14.48%, 68 heartbeats, sentinel-only LOOKUP_MISS) +
  visible title — must stay byte-identical to the F1 GATE A anchor.**
- **Single-CPU preserved.** F2 transfers the token to **exactly one** selected peer
  (`token_release_to` → `select_next_runnable`, priority + RR) before the holder continues; the holder
  either keeps running (no peer needs it AND no request) or yields to exactly one peer. At no point do
  two guest streams run. The fairness guard only reorders *which* single peer is chosen. The R2
  free-claim still admits exactly one claimant (serialized by `g_sched_mutex` + the while re-check,
  `:164-180`).
- **Generic.** No game addresses/constants. `g_handoff_requested` is armed by the generic event "a
  thread re-contends for the token while a peer holds it" — true for Patapon's sema-271 ring and
  `.hack`'s CriCond ring by the identical mechanism. `SCHED_PREEMPT_BUDGET` (20000) is the existing
  generic quantum. 271 / CriCond uids / `FUN_0886207C` are guest facts observed live, never written
  into the fix.

### 6.2 Option C (GE-interrupt window)
- **Deadlock-free.** The GE callback is non-blocking (`sched-d-ge-callback-rca.md` §2): it writes a
  guest ring, calls `SetEventFlag` under its own `ef->mtx`, optionally one indirect call, returns —
  never parks, never takes `g_sched_mutex`. So `enter → callback → leave` completes in bounded time and
  the exclusion is always released. The enter-wait is bounded by F2 (§4.4): `enter` arms
  `g_handoff_requested` + `g_ge_intr_pending`, so the holder void-releases at its next back-edge and
  `g_token_holder` reaches `nullptr` within one quantum. `g_sched_mutex` is DROPPED before the callback
  runs guest code, so `ef->mtx` never nests under `g_sched_mutex` (lock order preserved).
- **OFF byte-identical.** `sched_ge_interrupt_enter/leave` early-return on `!token_enabled()`;
  `g_ge_intr_active`/`g_ge_intr_pending` are never set OFF; the `token_acquire` `&& !g_ge_intr_active`
  conditions are only reachable ON (gated by `token_enabled()` at `:132`). The GE bracket runs the
  IDENTICAL `fn(rdram, &fin_ctx)` with the enter/leave as no-ops OFF — `psp_ge.cpp:237`/`:338` behave
  exactly as today. No emitter change. **Same OFF gate as §6.1.**
- **Single-CPU preserved.** While `g_ge_intr_active`, no PSP thread holds the token (`enter` waited for
  `nullptr`) and none can acquire it (the exclusion), so the GE callback is the SOLE guest execution —
  exactly single-CPU interrupt semantics: the interrupt takes the one CPU, runs to completion, returns
  it. The main thread never becomes `g_token_holder` and never appears in `g_threads[]`, so
  `select_next_runnable` / `g_rr_cursor` / the holder-parked assert (`:77`) are untouched.
- **Generic.** The bracket lives in the generic GE layer (`psp_ge.cpp`), keyed off the guest's own
  registered `g_finish_fn`/`g_signal_fn`. The scheduler primitive is title-agnostic. `fn=0x08816F9C`
  (Patapon's pointer) is the guest's data, never a literal in the fix.

---

## 7. Open concerns

1. **Emitter back-edge coverage of truly syscall-free spin regions.** F2 honors the forced hand-off
   only when the holder reaches `token_handoff` (via a syscall OR a `sched_preempt` back-edge). A
   guest loop with NO syscall AND no emitted back-edge preemption point would never reach the honor
   point. The Hazard-2 RCA's `[LISTLOOP]` spin is the suspect. Verify the emitter inserts
   `sched_preempt` at every loop back-edge in the wedge region; this is an emitter concern, not a
   scheduler one, but it gates F2's coverage of the tight-guest-loop path (§5 row 2).
2. **Fairness guard necessity is empirical.** §3.3 recommends including the `g_handoff_generation`
   guard from the start because the monopolization is deterministic, but plain directed-grant + the
   existing within-priority RR MIGHT converge on its own. Land F2, run GATE B/C; if `.hack` clears and
   Patapon `real_nonsprite` ramps WITHOUT the guard, the guard is unnecessary complexity — but the
   safe default is to include it (it is OFF-dead and removes the only ping-pong path). Measure
   `g_valve_nullptr_claims` (`:57`): a high/growing rate signals R2 re-claim ping-pong → guard needed.
3. **Over-yield under many threads.** `g_handoff_requested` is a single global flag; with 8 (Patapon)
   / 10 (.hack) threads, any peer un-park arms it, which may cause excessive hand-offs and throughput
   loss (re-introducing the slow-checkpoint cost). If GATE B shows `ge.frames` fps DROP vs F1-alone,
   refine to a strictly-higher-priority gate (only arm when the re-contending peer's priority <
   holder's) instead of any-peer. Start with the simple any-peer flag; measure hand-off frequency.
4. **DelayThread sleep-release must not regress audio/vblank timing.** Releasing the token across the
   DelayThread `sleep_for` (§5 row 3) is correct for single-CPU faithfulness, but verify the audio
   threads (which DelayThread-pace) still get their cadence — they were starved under F1
   (`WaitSema STUCK`), so F2 should IMPROVE them; confirm no new audio glitch.
5. **Mandatory `.hack` validation.** §3.4 argues convergence by mechanism; it MUST be proven on the
   `.hack` N≥30 freeze-rate harness (`build-hack/`, `PSPRECOMP_GAME=none`,
   `scripts/verify_determinism.sh dothack 30 75`). The F1 verifier showed ON 100% frozen at 2 frames —
   F2 must drop that toward 0 with boots progressing >100 frames. Not inferred — measured.
6. **GE SIGNAL bracket is untested live (Patapon fires 0 SIGNAL commands).** The SIGNAL bracket
   (`psp_ge.cpp:338`) is the same `FUN_08816F20` shape as FINISH, so the reasoning holds, but a title
   using GE SIGNAL is the validation vehicle for that branch.
7. **Re-run the FULL F1 gate suite after F2+C.** GATE A (Patapon OFF) must stay byte-identical; GATE
   C (.hack ON) freeze must drop toward 0; GATE B (Patapon ON) `real_nonsprite` must ramp toward the
   OFF band. Do NOT mark the single-runnable token done until BOTH .hack ON freeze drops AND Patapon
   ON `real_nonsprite` ramps off 0.

---

## 8. Ordered implementation steps

1. **F2 flag + set-site.** Add `static bool g_handoff_requested = false;` to `psp_scheduler.cpp`. In
   `token_acquire`, after the first-free check (`:152-155`), before the while-loop (`:164`), add
   `g_handoff_requested = true;` (the peer is re-contending while a foreign holder runs). ON-only by
   construction (the function early-returns OFF at `:132`).
2. **F2 honor-site.** In `token_handoff` (`:198`), change the F3 short-circuit (`:208`) to
   `if (!g_handoff_requested && !other_runnable_exists(self)) return;` and add
   `g_handoff_requested = false;` immediately before `token_release_to(self)` (`:211`).
3. **Build + GATE A (Patapon OFF byte-identical).** This must pass before anything ON is trusted —
   confirms the flag is OFF-dead.
4. **GATE B + GATE C-FAST with F2 only (no fairness guard, no DelayThread fix yet).** Measure whether
   plain directed-grant converges. Record `g_valve_nullptr_claims` and hand-off frequency.
5. **DelayThread sleep-release** (§5 row 3): bracket the `sleep_for` (`psp_hle_kernel_thread.cpp:307`)
   with `sched_token_release_for_wait()` / `sched_token_reacquire_after_wait()` (no object mutex held,
   so the bare after-wait re-acquire is F1-safe here). ON-only, OFF no-op. Re-run GATE B/C.
6. **Fairness guard** (§3.3, `g_handoff_generation` + per-thread `last_handoff_gen`) — add IF step 4/5
   show ping-pong (high `g_valve_nullptr_claims`, or .hack still frozen / Patapon `real_nonsprite`
   still 0). Recommended to include from the start given deterministic monopolization. Re-run GATE B/C.
7. **Option C primitives.** Add `g_ge_intr_active`, `g_ge_intr_pending`, `g_ge_intr_cv`,
   `sched_ge_interrupt_enter/leave` to `psp_scheduler.{h,cpp}`; add the `&& !g_ge_intr_active`
   conditions + `g_ge_intr_cv.notify_one()` at every void-release to `token_acquire`; make
   `token_handoff` void-release (not directed-grant) while `g_ge_intr_pending`.
8. **Option C bracket.** Wrap `psp_ge.cpp:237` (FINISH) and `:338` (SIGNAL) with
   `sched_ge_interrupt_enter()` / `sched_ge_interrupt_leave()`.
9. **GATE A again** (OFF byte-identical with C present) + **GATE B/C with F2+C**.
10. **.hack GATE C-FULL (N≥30)** — the promote gate. Freeze must drop toward 0, boots >100 frames.

### Recommended starting constants
- **Quantum:** keep `SCHED_PREEMPT_BUDGET = 20000` (`psp_scheduler.h:63`) — already the F4 perf value;
  it is the "held long enough" trigger for the back-edge `other_runnable_exists` arm. Only coarsen if
  step 4 shows over-yield throughput loss.
- **No new threshold for F2** beyond the boolean flag. The fairness guard, if added, needs no tunable —
  it is a generation comparison (`self`'s last-yield generation vs the request's generation).
- **GE enter-wait bound:** one `SCHED_PREEMPT_BUDGET` window (§4.4); no separate constant.

---

## 9. Structured summary
- **doc_path:** `.planning/research/sched-d-f2c-design.md`
- **forced_handoff_rule:** A thread that re-contends for the token while a DIFFERENT thread holds it
  (i.e. it just un-parked / was resumed / was signalled via SetEventFlag/SignalSema) sets
  `g_handoff_requested = true` inside `token_acquire` (`psp_scheduler.cpp`, after the first-free check
  `:152-155`, before the while-loop `:164`); honored in `token_handoff` (`:208`) which MUST hand off
  (skip the F3 sole-runnable keep) and clear the flag when it is set, falling back to the budget
  back-edge `other_runnable_exists` arm bounded by `SCHED_PREEMPT_BUDGET=20000`. Option C's
  `sched_ge_interrupt_enter` sets the SAME flag so a sole-runnable F3 holder is forced to release.
- **fairness_verdict:** **YES, a `g_rr_cursor`/`select_next_runnable` fairness fix is required in
  addition to F2.** ONE forced hand-off does not converge: the just-ran holder re-contends and can be
  re-selected (within-priority ping-pong) or re-claim the free token (R2, `:152`/`:176`) before the
  intended grantee runs, re-starving the consumer. The minimal generic fix is a `g_handoff_generation`
  guard so a holder cannot re-arm a forced hand-off on its own echo (it may only honor a request from
  a LATER peer event), keeping the just-ran holder from immediately preempting the new holder. Because
  the monopolization is deterministic (.hack 100% frozen), include the guard from the start; it is
  OFF-dead and removes the only remaining ping-pong path.
- **spin_paths_covered:** WaitSemaCB poll (covered — the poll thread void-releases every iteration and
  ARMS the flag against its producer on each re-acquire; it is the consumer, not the monopolizer);
  tight guest loop / clear-flip loop / `[LISTLOOP]` (covered via `sched_preempt` back-edges → honored
  in `token_handoff`, bounded by `SCHED_PREEMPT_BUDGET`, MODULO emitter back-edge coverage of truly
  syscall-free regions — open-concern §7.1); DelayThread `sleep_for` (**NOT covered by F2 alone —
  needs separate handling**: release the token across the host sleep, `psp_hle_kernel_thread.cpp:307`,
  since the holder runs no guest code during the real-time sleep and F2's flag cannot be observed while
  asleep); SleepThread/WaitThreadEnd (covered — they void-release before parking, no keep-across-sleep);
  render_queue round-trips (covered — already F1-compliant `release_for_wait`/`reacquire_unlocked`).
- **ordered_steps:** (1) add `g_handoff_requested` + set it in `token_acquire`; (2) honor it in
  `token_handoff`'s F3 site; (3) GATE A OFF byte-identical; (4) GATE B/C-FAST with F2 only, measure
  `g_valve_nullptr_claims` + hand-off freq; (5) DelayThread sleep token-release; (6) fairness guard
  (`g_handoff_generation`) if ping-pong shows; (7) Option C primitives + `token_acquire` exclusion;
  (8) bracket `psp_ge.cpp:237`/`:338`; (9) GATE A + B/C with F2+C; (10) .hack GATE C-FULL N≥30 promote.
- **open_concerns:** emitter back-edge coverage of syscall-free spins; fairness-guard necessity is
  empirical (include by default, measure `g_valve_nullptr_claims`); single global flag may over-yield
  with 8–10 threads (refine to higher-priority-only gate if fps drops); DelayThread sleep-release must
  not regress audio/vblank cadence; mandatory `.hack` N≥30 measured (not inferred); GE SIGNAL bracket
  untested live (Patapon fires 0 SIGNAL); re-run the full F1 gate suite after F2+C and do not mark done
  until `.hack` ON freeze drops AND Patapon ON `real_nonsprite` ramps off 0.

---

## 10. F2-core implementation notes (LANDED on `feat/single-runnable`)

**Scope of this increment:** ONLY F2-core — *fair within-priority token selection via just-ran
demotion*. The `g_handoff_requested` forced hand-off (§2, must-fix #1), Option C (GE-interrupt window,
§4), the DelayThread sleep-release (§5 row 3), and the emitter B-lite broadening are all DEFERRED to a
later increment. This increment changes ONLY the within-priority SELECTION ORDER; it never forces a
hand-off, never changes when `token_handoff` keeps vs releases. F3 (sole-runnable keep), R1
void-release, F1 un-nesting, and all existing behavior are untouched.

### 10.1 What was implemented

| edit | file | change |
|---|---|---|
| 1. per-thread stamp | `runtime/include/psp_scheduler.h` | new `uint64_t token_last_ran = 0;` on `PspThread` — the value of `g_token_grant_seq` when this thread was last GRANTED the token |
| 2. global grant seq + helper | `runtime/src/psp_scheduler.cpp` | new file-static `uint64_t g_token_grant_seq = 0;` (under `g_sched_mutex`) + `token_mark_granted(t)` which bumps the seq and stamps `t->token_last_ran` |
| 3. stamp at every grant site | `runtime/src/psp_scheduler.cpp` | `token_mark_granted()` called at all three non-null `g_token_holder = X` sites: `token_release_to` directed grant (`:164`), `token_acquire` first-free fast path (`:200`), `token_acquire` R2 free-claim (`:225`) |
| 4. within-priority tiebreak | `runtime/src/psp_scheduler.cpp` | `select_next_runnable` now keeps "highest priority (lowest number) first" as the PRIMARY key, and breaks within-priority ties by SMALLEST `token_last_ran` (least-recently-ran wins). Strict `<` preserves the `g_rr_cursor`-ordered first match on equal stamps, so `g_rr_cursor` stays as the SECONDARY tiebreak / determinism anchor (retired-in-spirit but kept for stable rotation among equal-stamp peers, e.g. all-0 at startup) |

**Effect:** a just-yielded holder is stamped with the LARGEST `token_last_ran` and is therefore picked
LAST among its priority; a starved / freshly-un-parked peer (smaller or 0 stamp) is picked FIRST. This
closes the design's must-fix #1 *re-selection* ping-pong: a yielded holder that stays `status==RUNNING`
and `token_parked==false` is no longer immediately re-selectable ahead of a peer that was waiting longer.

### 10.2 Per-edit OFF-identity argument (flag-OFF stays byte-identical)

- **Edit 1 (`token_last_ran` field):** initialized to `0` in the struct default-member-initializer
  (same place `token_parked` is). OFF it is NEVER written (the only writer is `token_mark_granted`,
  reachable only via the three grant sites, all inside `token_acquire`/`token_release_to` which
  early-return at `if (!token_enabled()) return;`) and NEVER read (the only reader is
  `select_next_runnable`, which is unreachable OFF — see Edit 4). A new always-zero struct field with
  no OFF reader/writer cannot change OFF behavior.
- **Edit 2 (`g_token_grant_seq` + `token_mark_granted`):** the global starts at 0 and is bumped only
  inside `token_mark_granted`, which is called only from the three grant sites, each downstream of a
  `token_enabled()` early-return. OFF the counter never moves and the helper is never called.
- **Edit 3 (stamp calls at grant sites):** `token_release_to` returns at `if (!token_enabled()) return;`
  before the directed-grant stamp; `token_acquire` returns at the same gate before the first-free and
  R2 stamps. OFF none of the three `token_mark_granted()` calls is reached.
- **Edit 4 (`select_next_runnable` tiebreak):** `select_next_runnable` is reached ONLY from
  `token_release_to`, `other_runnable_exists`, and `token_acquire` — every one of which early-returns at
  the `token_enabled()` gate before calling it. OFF the function is never entered, so the new
  least-recently-ran branch (and the `token_last_ran` read) is dead OFF. The OFF `sched_yield_point`
  path (`:388-434`) uses its OWN inline priority scan and never touches `select_next_runnable`. The
  tiebreak change is OFF-invisible by construction.

**Net:** every new state write and the new selection branch sits behind the existing `token_enabled()`
gate. OFF, `token_last_ran` stays 0, `g_token_grant_seq` stays 0, and `select_next_runnable` is never
entered — the OFF path is byte-identical to the pre-F2-core tree. (Smoke-confirmed: Patapon flag-OFF
healthy — frames ramp, `real_nonsprite` ramping 5526→5767, reached GRAPHICS, sentinel-only LOOKUP_MISS,
no crash.) Determinism is preserved: stamps are assigned deterministically and strict-`<` keeps the
deterministic `g_rr_cursor` order on ties. Single-CPU is preserved: this is a selection-ORDER change
only — `token_release_to` still transfers to exactly one peer; never two holders.

### 10.3 Smoke results — does the routing fix work?

`cargo test -q`: **299 passed, 0 failed.** Both runtimes built clean (`runtime/build` Patapon,
`build-hack` GAME=none for .hack).

- **Patapon flag-OFF (~18s):** HEALTHY. ge.frames=382, prims=6221, `real_nonsprite=5840` (ramping
  toward the 13.8k–15.7k band), 8 threads up, 24 GRAPHICS/heartbeat lines, LOOKUP_MISS sentinel-only,
  no crash. OFF unchanged.
- **Patapon flag-ON (~150s, `PSPRECOMP_PREEMPT=1 PSPRECOMP_CROSS_MID=1 PSPRECOMP_CLEANROOM=1`):**
  **THE GATE — PASSED.** `real_nonsprite` RAMPS OFF 0 (vs F1's stuck-at-0-for-180s): observed
  trajectory 0 → 34 → 275 → 516 → … → 4144 → **8585 and climbing**. ge.frames ramped 1244→1789; **194
  `clear=0` (real-geometry) draws appeared in the log** (vs F1's ZERO). `recent_funcs` shifted off the
  frozen `0x08815FB4` clear/flip cluster to the GE-geometry helpers (`0x089B4B94`/`0x088D808C`/…) —
  the geometry producer now gets token time. lookup_miss sentinel-only, no crash, 40 heartbeats.
  **The just-ran demotion routes the token off the monopolizing clear/flip holder to the un-parked
  geometry producer.** Patapon routing: FIXED.
- **.hack flag-ON (`scripts/verify_determinism.sh dothack 8 75`, `PSPRECOMP_PREEMPT=1
  PSPRECOMP_CROSS_MID=1`):** **NOT fixed by F2-core alone — freeze-rate 8/8 = 100.0%, every boot stuck
  at 2 frames** (no change from F1). Diagnostic snapshot: all CRI consumer threads `RUNNING wait=''`
  (SPINNING, NOT `token_parked`), `recent_funcs` head frozen on the same CRI cluster
  (`0x08837978,0x0883B2B8,0x08815E70,…`), 110 `SetEventFlag` (producer firing) + 61 `WaitEventFlag`
  (consumer) with zero forward progress. **Why F2-core does not fix .hack:** the .hack wedge is a
  producer/consumer ring-CONVERGENCE problem, not a single-thread MONOPOLY. The consumers are spinning
  (not parked), so least-recently-ran rotation already rotates the token among them — but the ring
  advances only when a `WaitEventFlag` consumer drains AFTER the producer's `SetEventFlag`, and that
  ORDERING requires the deferred `g_handoff_requested` forced hand-off (must-fix #1, §2: a consumer
  whose predicate fires must ARM a forced hand-off against the producer) — exactly the mechanism scoped
  OUT of this F2-core increment. Just-ran demotion fixes the *monopoly* (Patapon); the *forced
  hand-off* fixes the *ring convergence* (.hack). They are complementary; F2-core is the monopoly half.

### 10.4 Verdict

`routing_fixed` = **PARTIAL/true-for-the-monopoly-case.** F2-core fixes the within-priority routing
starvation that the design targets (Patapon `real_nonsprite` ramps off 0; the just-ran holder is
demoted and the starved producer is selected first — the exact must-fix #1 *re-selection* defect). It
does NOT clear the .hack CriCond ring freeze, which needs the deferred `g_handoff_requested` forced
hand-off (the *ordering*/convergence half of F2, §2) and/or Option C. OFF stays byte-identical
(per-edit argument §10.2; Patapon OFF smoke healthy). Next increment: land `g_handoff_requested` (§2)
to close .hack, then Option C (§4) and B-lite as hardening. Full N≥30 acceptance + the Patapon
CLEANROOM band is the separate verifier's job; this increment establishes the trend (Patapon ON ramps,
.hack ON unchanged-pending-forced-handoff).
