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

Started automatically (`runtime/src/psp_debug_socket.cpp`). One command per line via `nc`;
multiple concurrent clients supported. **v2 framing:** every command replies
`OK <len>\n` + `<len>` payload bytes, or `ERR <reason>\n` — malformed input is never
silently dropped. Full protocol details in the issue #35 section below.

| Command | Effect | Reply |
|---|---|---|
| `I` | Runtime info (uptime, GE counters, LOOKUP_MISS, recent funcs, threads) | `OK <len>` + one JSON line |
| `R <hexaddr> <decsize>` | Read masked PSP memory (≤65536 bytes; out-of-range zero-filled) | `OK <size>` + raw bytes |
| `RAW <hexaddr> <decsize>` | Legacy v1 read — unframed | raw bytes only |
| `W <hexaddr> <hexbytes>` | Write bytes into rdram (masked; whole range must be in bounds) | `OK 0` |
| `B <hexmask> <decms>` | Inject button mask for duration (UP=10, CROSS=4000, START=8; clamped 60 s) | `OK 0` |
| `S <path>` | Screenshot: render thread writes 480x272 TGA to `<path>` | `OK 0` (≤10 s) or `ERR timeout` |

```bash
printf 'B 10 250\n'   | nc 127.0.0.1 9999   # UP
printf 'B 4000 250\n' | nc 127.0.0.1 9999   # CROSS
printf 'I\n' | nc -w 3 127.0.0.1 9999 | tail -n +2 | jq .   # info as JSON
```

Keyboard (window focus required): arrows=D-pad, X/Enter=CROSS, Z=CIRCLE, A=SQUARE,
S=TRIANGLE, Q/W=L/R, Space=START, Tab=SELECT. Buttons auto-release on focus loss — prefer
the socket for scripted/agent input.

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

## #35 — Debug socket v2 (ACK framing, info JSON, write, screenshot, multi-client)

**What was added** (`runtime/src/psp_debug_socket.cpp`, hooks wired in `runtime/src/main.cpp`):

- **ACK framing.** Every command replies `OK <len>\n` followed by exactly `<len>` payload
  bytes, or `ERR <reason>\n` with no payload. Malformed/unknown input always gets an ERR
  (v1 dropped it silently, which made scripted probing indistinguishable from a hang).
- **Multi-client.** Thread-per-client accept loop; concurrent `nc` sessions are independent
  (v1 served one client at a time — a second connect hung until the first closed).
- **Legacy compat choice:** v2 frames `R`; the v1 unframed read survives verbatim as the
  `RAW <hexaddr> <decsize>` alias (success replies are raw bytes with no header; malformed
  RAW still gets ERR). Old clients switch `R`→`RAW`, new clients use framed `R`.

### Command reference

| Command | Args | Success reply | ERR reasons |
|---|---|---|---|
| `I` | — | `OK <len>` + one newline-terminated JSON line | — |
| `R` | `<hexaddr> <decsize>` (1..65536) | `OK <size>` + raw bytes; masked `0x07FFFFFF`; out-of-range tail zero-filled | `bad-addr`, `bad-size`, `size-out-of-range` |
| `RAW` | same as `R` | raw bytes, **no header** (v1 behavior) | same as `R` |
| `W` | `<hexaddr> <hexbytes>` (even-length hex, no spaces) | `OK 0` | `bad-addr`, `bad-bytes`, `out-of-range` (whole range must fit — no partial writes) |
| `B` | `<hexmask> <decms>` (clamped 60 s) | `OK 0` | `bad-mask`, `bad-duration` |
| `S` | `<path>` (rest of line, spaces allowed) | `OK 0` after the TGA is on disk | `bad-path`, `unsupported`, `timeout` (10 s; also covers pre-GL boot and a concurrent capture in flight) |
| anything else | | | `unknown-command`, `empty`, `line-too-long` (>4095 chars; connection stays usable) |

### `I` JSON schema (one line, jq-able)

```json
{"uptime_sec": 70.9,
 "ge": {"frames": 1667, "prims": 12696, "real_nonsprite": 11030,
        "sprite_nonclear": 0, "clears": 1666},
 "lookup_miss": {"unique": 1, "total": 1},
 "recent_funcs": ["0x089B440C", "..."],
 "threads": [{"id": 0, "name": "user_main", "status": "RUNNING", "wait": ""},
             {"id": 2, "name": "sgx-psp-freq-thr", "status": "RUNNING", "wait": "sema:278"}]}
```

- `ge.*` mirrors the `[GE_GEOM_*]` sentinel counters (`real_nonsprite > 0` == GRAPHICS).
- `recent_funcs` is a shared 64-entry ring fed by every recompiled-function entry,
  oldest first — entries from all game threads interleave, so cross-thread ordering is
  approximate. (The per-thread PC-TRACE ring is `thread_local` and not readable here.)
- `threads[].status` ∈ DORMANT/READY/RUNNING/WAIT/DEAD/WAIT_SLEEP. `wait` is `sema:<uid>` /
  `semacb:<uid>` / `sleep` / `thread_end:<id>` / `""`. **Caveat:** sema waits block on the
  sema's own condvar without changing scheduler status, so a sema-blocked thread shows
  `status:"RUNNING"` with a non-empty `wait` — trust `wait` over `status` for "is it stuck".
- All reads are deliberately racy snapshots (documented in code); never assert on exact counts.

```bash
printf 'I\n' | nc -w 3 127.0.0.1 9999 | tail -n +2 \
  | jq '{frames: .ge.frames, waits: [.threads[] | select(.wait != "") | {name, wait}]}'
printf 'W 9FFFF00 DEADBEEF\n' | nc -w 3 127.0.0.1 9999       # OK 0
printf 'R 9FFFF00 4\n' | nc -w 3 127.0.0.1 9999 | xxd        # OK 4 + dead beef
printf 'S /tmp/frame.tga\n' | nc -w 12 127.0.0.1 9999        # OK 0 once written
```

**Screenshot path obeys the GL rule:** the socket thread only posts a request; the main
(GL) thread polls `ge_draw_service_screenshot_request()` each event-loop pass next to
`render_queue_process()` and does the FBO readback there. Give `nc` a `-w` ≥ the service
latency (capture is usually <50 ms once GL is up; the 10 s bound covers pre-GL boot).

**Failure modes discovered during verification:**
- `S` before `ge_draw_init` (first ~1 s of boot) fails fast → `ERR timeout`; retry later.
- v1's `strtoul` parsing accepted garbage as address 0 — v2 rejects empty/garbage tokens
  explicitly (`bad-addr`/`bad-size`), so don't rely on `R zz 4` reading address 0 anymore.
- `W` refuses partially-out-of-range writes entirely (no partial write), unlike `R` which
  zero-fills — asymmetry is intentional.

**How verified:** `runtime/build/test_debug_socket` (59 checks: framing, ERR paths, W/R
round-trip, masking, JSON shape + hostile-name escaping, two concurrent clients, oversized
line) plus a live run against Patapon — `I` during boot and at title, `W`+`R` round-trip at
`0x09FFFF00`, `S` produced the actual 480x272 title-screen TGA mid-run, `B 4000 250` → `OK 0`,
malformed commands → ERR, two simultaneous `nc` sessions served concurrently.
<!-- #36 build fingerprinting: section added by its implementation -->
## #37 — recompile_report.json (silent-path audit)

Every `psprecomp recompile` run writes `<output>/recompile_report.json` and prints a
one-paragraph summary. The report turns the pipeline's four historically *silent* failure
paths into named, counted artifacts — read it first when triaging a new game or an
unexplained runtime divergence.

### The four silent paths it exposes

| Path | What used to happen | Report field |
|------|--------------------|--------------|
| Unhandled relocation types | `reloc.rs` left the word untouched for anything other than R_MIPS_32/26/HI16/LO16 — unrelocated pointers with no trace | `unhandled_relocations` (r_type → count); also warned once per type at analyze time |
| Unresolved NIDs | a database miss silently became a `NID_0x%08X` stub name | `unresolved_nids[]` (nid, stub_addr, fallback_name) |
| Decode errors | function silently emitted as an empty stub with a comment | `decode_errors[]` (address, name, error) |
| Unbacked static dispatch targets | emitter generated `RECOMP_LOOKUP(0xADDR)` for a target absent from the dispatch table — a guaranteed LOOKUP_MISS if reached | `dispatch_audit.missing_targets[]` |

### Schema (schema_version 1)

Top-level keys: `schema_version`, `generated_at` (ISO-8601 UTC), `counts`,
`decode_errors[]`, `unresolved_nids[]`, `unhandled_relocations{}`, `dispatch_audit{}`,
`dedup_renames[]`. All addresses are hex strings (`"0x%08X"`, analysis.json style);
`unhandled_relocations` keys are decimal r_type values as strings.

`counts` carries the self-check numbers: `functions_total`, `functions_by_source`
(analysis.json `source` field), `discovery` (recompile-stage pass breakdown:
force/raw_scan/prologue/gap_start/gap_rescued), `mid_entries`, `batch_files`,
`imports_total`, plus a total per silent-path category. `dedup_renames[]` lists every
function the emitter renamed with the `_ADDR` suffix for ODR safety (the dispatch/funcs.h
name to grep for is `unique_name`, not the Ghidra name).

Authoritative schema doc: module header of `crates/psp-cli/src/report.rs`.

### Count assertions (--expect flags)

```bash
PSPRECOMP_CROSS_MID=1 cargo run --release -- recompile analysis.json -o output \
    --expect-functions 14104 --expect-mid-entries 2022   # Patapon baseline
```

On mismatch, recompile exits nonzero with a clear error — *after* writing the report, so
the counts breakdown is available for triage. Both flags are optional; use them in scripts
and verification gates to turn the documented baseline into an assertion instead of a
doc-comment that drifts.

### Triaging a new game with the report

1. Recompile without expect flags; read the printed summary line.
2. `unhandled_relocations` non-empty (PRX games) → relocation support gap; affected code
   reads unrelocated pointers. Fix in `psp-parser/src/reloc.rs` before chasing runtime bugs.
3. `unresolved_nids` non-empty → update the NID database; each listed stub is an import the
   runtime can only no-op. (ELF games like Patapon have an empty analysis.json `imports`
   array — their stubs are discovered from `.lib.stub` at analyze time, so this list covers
   PRX-style imports.)
4. `decode_errors`: cluster the addresses. A contiguous run of entries with ASCII-looking
   words (e.g. `0x44555453` = "STUD") is data misdetected as code by heuristic discovery —
   harmless stubs. A decode error in a *Ghidra-sourced* function is a real decoder gap.
5. `dispatch_audit.missing_targets`: targets **inside** the loaded segment range are real —
   each is a guaranteed LOOKUP_MISS if reached (cross-check against runtime LOOKUP_MISS
   logs). Targets outside the segment range are artifacts of data words misdecoded as `j`
   inside heuristically recovered functions (same root cause as the decode-error cluster).

### Verified (2026-06-12, Patapon BOOT.BIN analysis.json)

Full recompile with `PSPRECOMP_CROSS_MID=1`: counts matched the documented baseline
(14,104 functions / 2,022 mid-entries / 283 batch files) and the expect flags passed;
a deliberate `--expect-functions 99` run exited 1 with the mismatch error. The report
contained the 4 known `FUN_089DBC*` decode errors among 1,830 total (1,829 from
recompile-stage heuristic discovery decoding data, 1 from hle_scan). The dispatch audit
found 346 missing targets — **all** at `0x02xxxxxx`, none inside the loaded segment
(0x08804000–0x08ADC400): garbage `j`-targets decoded from data words, not real misses.
Zero unresolved NIDs and zero relocations (Patapon is a relocation-free ELF; both paths
exercised by unit tests instead).

## #38 — `dump <analysis.json> 0xADDR` (single-function C++ emission)

Emits exactly one function's generated C++ to stdout — no full recompile, no grepping
batch files, no writes under `output/`. The list modes are unchanged
(`dump analysis.json functions|imports|relocations|segments|mid_entries`, positional or
`--what`; default `functions`).

```bash
# Module start (named "entry" in emitter output)
PSPRECOMP_CROSS_MID=1 cargo run --release -- dump analysis.json 0x089ACCD0

# Ordinary function; 0x prefix optional, hex case-insensitive
PSPRECOMP_CROSS_MID=1 cargo run --release -- dump analysis.json 0881e7a8 > /tmp/f.cpp

# Mid-entry address -> actionable error on stderr, exit 1:
#   Error: 0x08804CE8 is a mid-entry inside FUN_08804a4c (entry 0x08804A4C); ...
#   dump the parent: psprecomp dump <analysis.json> 0x08804A4C
cargo run --release -- dump analysis.json 0x08804CE8
```

Address resolution is against the **post-pipeline** function list (after discovery,
force-injection, and the CROSS_MID passes). Non-entry addresses never silently emit the
wrong thing: a registered mid-entry names its parent; an address strictly inside a body
names the containing function; an unknown address lists the nearest entries below/above;
bad hex is a usage error. Errors and all tracing go to stderr — stdout carries only C++,
so it pipes/redirects cleanly.

**Fidelity / CROSS_MID note:** the dump goes through the same `prepare_emission` +
`emit_one_function` path as the batch emitter (same IR, same global `_ADDR` name dedup,
same mid-entry dispatch), so its output is **byte-identical** to the function's text in
its `batch_*.cpp` for the same flag set. That means `PSPRECOMP_CROSS_MID` (and
`PSPRECOMP_NO_COALESCE`/`PSPRECOMP_NO_RA_MODEL`, and `--config`) must match whatever the
`output/` you are comparing against was generated with — CROSS_MID changes coalescing and
mid-entries, which changes bodies. Pipeline prep takes a few seconds (it re-runs discovery,
and under CROSS_MID the coalesce/recovery decode passes) before the single function emits.

**How verified:** unit tests in `crates/psp-cli/src/dump.rs` compare the dump path against
in-memory batch emission for the real analysis.json (entry, an ordinary `FUN_`, a mid-entry
parent, an `_ADDR`-deduplicated thunk — all byte-identical substrings), plus end-to-end CLI
runs with `PSPRECOMP_CROSS_MID=1` on both sides: dumps of `entry` (0x089ACCD0, 4052 bytes),
mid-entry parent `FUN_08804a4c` (11637 bytes), dedup `thunk_FUN_08864f98_0887f1c4`, and
`FUN_0881e7a8` were each exact byte substrings of their `output/generated/batch_*.cpp`,
with the recompile baseline (14104/2022/283) unchanged.
