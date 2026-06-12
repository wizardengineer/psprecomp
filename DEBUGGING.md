# DEBUGGING.md — psprecomp debugging methodology

The single home for project-specific debugging practices, workflows, commands, failure
patterns, and lessons learned. CLAUDE.md points here; new debugging knowledge goes here,
not into CLAUDE.md. Distilled from ~48 recorded debugging sessions
(`.planning/handoff/next-session/`) and the 2026-06 infrastructure research
(`.planning/research/debug-infrastructure-research.md`).

Audience: humans **and** AI agents. Every workflow below is scriptable without window focus
or GUI interaction unless explicitly noted.

---

## 1. Core principles (violating these has cost real sessions)

1. **No fix without a measured store/branch/value.** Guessing the subsystem ("it's probably
   the X system") produced zero pixels across four phases once. Find the instruction or
   write that diverges first.
2. **Same-snapshot only.** Never track an absolute heap address across boot phases —
   addresses get reused; only within-snapshot diffs are trustworthy. Re-derive pointer
   chains per boot (double-deref!).
3. **CLEANROOM for verification.** `PSPRECOMP_CLEANROOM=1` disables register-mutating shims.
   Band-aids fabricate phantom blockers; always reproduce in the clean room before believing
   a divergence is real.
4. **A probe that doesn't replicate the game's own access conditions will lie.** Verify a
   surprising result a second, independent way before acting on it.
5. **Don't conclude "never" from one run.** Boot is nondeterministic; use ≥120 s windows and
   multiple runs.
6. **Anti-cascade.** If a fix only reveals the next layer, characterize it, file/comment the
   issue, and stop — use the oracle to find the *first* divergence instead of grinding.
7. **Define gates operationally** — by the exact log line and code line, never by a human
   label ("has 3D geometry" once meant something else than its label).
8. **Logs are radioactive.** Trace-enabled runs reach multi-GB. Always `timeout`, grep,
   delete. Never `cat` a runtime log.
9. **One dynamic resource, one agent.** One SDL window, one debug-socket port, one build
   tree. Parallel agents must use worktrees (symlink `disc0/` and `analysis.json`) and kill
   only their own binary path.
10. **Distrust prior conclusions without evidence links.** Six sessions were misled by stale
    handoff framings. Check `.planning/handoff/next-session/<highest>-START-HERE.md` §0
    corrections and §8 don't-re-chase before re-opening any closed lead.

## 2. Build & verification commands

```bash
# Rust pipeline
cargo build --release && cargo test --release
PSPRECOMP_CROSS_MID=1 cargo run --release -- recompile analysis.json -o output

# Runtime (Release for verification, Debug for lldb)
cmake -B runtime/build -S runtime && cmake --build runtime/build -j$(sysctl -n hw.ncpu)
cmake -B runtime/build-debug -S runtime -DCMAKE_BUILD_TYPE=Debug
cmake --build runtime/build-debug -j$(sysctl -n hw.ncpu)

# Unit gates
./runtime/build/test_vfpu                                              # 89/89
DATA_CMN_BND_PATH=disc0/PSP_GAME/USRDIR/DATA_CMN.BND ./runtime/build/test_asset_bnd  # 33/33

# CLEANROOM verification run (the canonical gate)
PSPRECOMP_CROSS_MID=1 PSPRECOMP_CLEANROOM=1 timeout 120 \
  ./runtime/build/psprecomp_runtime > /tmp/run.log 2>&1
# Gates: exit 124; sentinel-only LOOKUP_MISS; 0 "bus error"/"marking dead";
# [GE_GEOM_HEARTBEAT] monotonic; real_nonsprite in expected band (~13.8-14.1k @120s);
# "proj matrix degenerate" ABSENT; "view matrix all-zero" exactly once.
# DELETE /tmp/run.log after grepping.
```

**Stale-output trap:** `output/` is gitignored. After ANY emitter change you MUST
regenerate (`recompile`) before building the runtime, and `PSPRECOMP_CROSS_MID=1` must be
set at recompile time. Symptom of staleness: measurements that contradict the code you just
changed. (Issue #36 adds fingerprint checking for this.)

## 3. Triage by symptom

| Symptom | First moves |
|---|---|
| **LOOKUP_MISS** | Function not in dispatch table. Check analysis.json; `[LOOKUP_MISS_CTX]` dumps args + 32-entry func ring; `PSPRECOMP_STRICT=1` aborts at first miss for a backtrace. |
| **SIGSEGV** | Address-masking issue. Check the access is within PSP_MEM_SIZE (128MB, mask `0x07FFFFFFU`). lldb on the Debug build. |
| **Hang/stall** | `lldb` → `process interrupt` → `bt`; `psp_threads` (custom command) for PSP thread wait states. Mid-entry dispatch: check `ctx->entry_point` set/cleared. Sema STUCK warns appear rate-limited. |
| **Wrong branch / wrong data** | Oracle-driven differential debugging (§4). Hardware watchpoint on "the value that should change but doesn't". |
| **No render / wrong render** | `PSPRECOMP_GE_TRACE=1`; geometry sentinels (`[GE_GEOM_VERDICT]`, heartbeat); matrix fallback notices; `PSPRECOMP_SCREENSHOT=/tmp/f.tga`; compare vs PPSSPP frame. |
| **Build failure in `output/generated/`** | Fix the Rust emitter and regenerate — NEVER edit generated files. |
| **Linker errors** | funcs.h vs batch names; `_ADDR` dedup suffixes; CMake glob must be `batch_*.cpp` only. |

## 4. Oracle-driven differential debugging (the method for hard data/render bugs)

PPSSPP is a scriptable behavioral oracle, not a passive reference:

1. **Drive it**: launch GUI PPSSPP with the game image (debugger WebSocket auto-listens —
   find the port: `lsof -i -P | grep -i PPSSPP | grep LISTEN`). `game.reset` arms
   breakpoints before one-time boot events; `input.buttons.press` advances menus. Some
   builds drop the `cpu.resume` reply — poll `cpu.status`.
2. **Capture ground truth**: exec-bp a function to read args; one-shot-bp the caller's `ra`
   for return values; memory-write-bp a field to find its writer.
3. **CAVEAT**: the current PPSSPP build **crashes under exec breakpoints/stepping**
   (CoreAudio). Poll-only: memory reads, `press`, memory-write `watch`. Relaunch with the
   ISO to replay boot.
4. **Diff same-snapshot only** (principle 2).
5. **psp-reclass** (`~/Projects/PersonalWork/psp-reclass`): dual-target memory bridge
   (PPSSPP WebSocket + our debug socket): `read`, `read-struct`, `follow` (pointer chains),
   `diff`, `watch`, `press`; `--json` on everything for agents.
6. **A/B with git stash** settles "what did this change really do" disputes decisively.
7. **Differential unit harness**: when an operation's semantics are in doubt, build a tiny
   host harness linking our implementation against PPSSPP's source semantics (the VFPU bug
   was pinned this way without any emulator run).

## 5. lldb recipes

Launch, don't attach (macOS denies attach). `runtime/.lldbinit` auto-loads
`runtime/tools/psp_lldb.py` (`psp_thread`, `psp_threads`, `psp_module`, `psp_sema`,
`psp_heap`). `g_rdram` symbol is `__ZL7g_rdram`.

```bash
lldb ./runtime/build-debug/psprecomp_runtime
(lldb) b FUN_0881E7A8                                   # recompiled function
(lldb) b hle_sceKernelCreateThread                      # HLE stub
(lldb) p/x ctx->r[4]                                    # a0 (r[2]=v0, r[29]=sp, r[31]=ra)
(lldb) memory read rdram+(0x089F69B4&0x07FFFFFF) -c 32  # PSP memory (masked)
(lldb) b FUN_0881E558 -c 'ctx->r[4] == 0x09000000'      # conditional
(lldb) watchpoint set expression -- rdram+(0x089F69B4&0x07FFFFFF)  # write watch
(lldb) process interrupt                                 # catch a stall
```

Landmines: counting breakpoint *hits* must grep `frame #0:` lines only (command echoes were
once counted as hits); a `/tmp/dis.py` shadowing a stdlib module once corrupted lldb reads;
`PC_TRACE`'s atexit dump won't fire on SIGKILL (heartbeat sentinels are fflushed and do
survive).

**Tripwire pattern** for rare, lldb-suppressed races: plant a one-shot in-process state dump
at the symptom site and let normal runs capture it.

## 6. Runtime debug socket (TCP 127.0.0.1:9999)

Started automatically (`runtime/src/psp_debug_socket.cpp`). One command per line via `nc`.

| Command | Effect |
|---|---|
| `R <hexaddr> <size>` | Read masked PSP memory, raw bytes |
| `B <hexmask> <ms>` | Inject button mask for duration (UP=10, CROSS=4000, START=8; clamped 60 s) |

```bash
printf 'B 10 250\n'   | nc 127.0.0.1 9999   # UP
printf 'B 4000 250\n' | nc 127.0.0.1 9999   # CROSS
```

Keyboard (window focus required): arrows=D-pad, X/Enter=CROSS, Z=CIRCLE, A=SQUARE,
S=TRIANGLE, Q/W=L/R, Space=START, Tab=SELECT. Buttons auto-release on focus loss — prefer
the socket for scripted/agent input.

(Issue #35 extends this protocol — its section below documents the v2 commands.)

## 7. Trace environment flags

| Flag | Effect |
|---|---|
| `PSPRECOMP_STRICT=1` | Abort on LOOKUP_MISS / unimplemented NID |
| `PSPRECOMP_CLEANROOM=1` | Disable all behavioral shims (verification mode) |
| `PSPRECOMP_HLE_TRACE=1` | Per-call HLE arg/ret logging (`[HLE-TRACE]`) |
| `PSPRECOMP_PC_TRACE=1` | Per-function frequency counters + atexit top-20 |
| `PSPRECOMP_GE_TRACE=1` | GE command-stream logging |
| `PSPRECOMP_SEMA_TRACE=1` | Per-call sema logging (off by default — floods ~1 GB/min) |
| `PSPRECOMP_SCREENSHOT=path` | One-shot TGA after first prims + shutdown capture |
| `PSPRECOMP_SPLEAK=1` | Shadow-stack sp-leak detector |
| `PSPRECOMP_CROSS_MID=1` | (recompile-time) cross-function mid-entry emission — must match workflow |

~30 more narrow investigation probes exist (LK_*, GK_*, BND_*, ...); they are one-off
band-aids/probes from past bugs — discover via `rg 'getenv\("PSPRECOMP_' runtime crates`,
and prefer not to rely on them (issue #43 tracks a proper registry/channel system).

## 8. Static-side oracles

- **pyghidra-mcp** (BOOT.BIN in a Ghidra project): `decompile_function`,
  `list_cross_references`, `search_symbols_by_name`, `gen_callgraph`, `search_code`.
- `cargo run --release -- dump analysis.json <mode>` for analysis.json inspection
  (see issue #38 section for single-function C++ dumps).

## 9. Known failure patterns (don't re-discover)

- **PPSSPP crashes when paused/stepping** — poll-only oracle work (§4).
- **Stale `output/`** after emitter changes — §2 trap; regenerate first.
- **`g_last_func_addr` in HLE logs is NOT a return address** — it's the last dispatched
  function; misread twice historically.
- **Band-aid stacks fabricate phantom blockers** — verify in CLEANROOM.
- **lv.q/VFPU register-file class bugs live in the runtime register file**, not the
  emitter/decoder (verified faithful against raw BOOT.BIN words).
- **The freq-que/at3 audio workers parked on their reqsemas is NORMAL idle state.**
- **`verify_geometry.sh`-style oracles need a self-test path** (a synthesized known-good
  draw) to prove they're not constant-false.

---

# Per-issue infrastructure notes

Sections below are added by the issue that introduced the infrastructure. Each section:
what was added, how to use it, how it was verified, and any new failure modes discovered.

<!-- #35 debug socket v2: section added by its implementation -->
<!-- #36 build fingerprinting: section added by its implementation -->
<!-- #37 recompile report: section added by its implementation -->
<!-- #38 dump single-function: section added by its implementation -->
