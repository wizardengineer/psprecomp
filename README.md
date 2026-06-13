# psprecomp

A PSP static recompiler: a Rust pipeline translates Allegrex MIPS32 binaries into C++17 source,
and a C++17 runtime (SDL2 + OpenGL 3.3) executes the result with HLE implementations of the PSP
OS services. The target binary is **Patapon (USA) BOOT.BIN**.

> [!WARNING]
> **One game only.** This project has been developed and verified against exactly one binary —
> Patapon (USA). HLE semantics, renderer fallbacks, and asset handling may be specific to it;
> other PSP games will most likely not work without additional effort.
>
> **Vibe coded.** The codebase is largely AI-generated (Claude). Every change is verified by
> running the game and diffing behavior against PPSSPP, but the code has not had a traditional
> human review pass — expect rough edges.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the structural reference (crates, runtime subsystems,
data flow, invariants), and [docs/GRAPHICS.md](docs/GRAPHICS.md) for how the runtime translates
the PSP Graphics Engine to OpenGL 3.3.

## Table of Contents

- [Overview](#overview)
- [Pipeline](#pipeline)
- [Why Ghidra?](#why-ghidra)
- [Current Status](#current-status)
- [Building and Running](#building-and-running)
- [Verification Methodology](#verification-methodology)
- [Limitations](#limitations)
- [Reference Projects](#reference-projects)
- [License](#license)

## Overview

The recompiler works in two stages. `psprecomp analyze` runs Ghidra headless analysis on the PSP
ELF and merges it with a Rust ELF parser into `analysis.json` (functions, imports, relocations,
mid-function entry points, xrefs, data sections). Both PSP executable formats are handled:
ET_EXEC binaries (Patapon's BOOT.BIN) load where linked, and relocatable PRX modules
(decrypted EBOOT.BIN, `e_type 0xFFA0`) are rebased to the PSP user-module base 0x08804000 with
their Type-A relocation tables applied and their imports parsed from SceModuleInfo.
`psprecomp recompile` decodes every instruction
into a typed IR and emits C++17: one C++ function per guest function, an address-to-function
dispatch table, and the data sections. The generated code compiles together with the runtime in
`runtime/`, which provides guest memory, a cooperative thread scheduler, HLE stubs for the
imported firmware NIDs, and a PSP Graphics Engine (GE) to OpenGL 3.3 translation layer.

For Patapon BOOT.BIN the current pipeline recompiles ~14,104 functions with 2,022 mid-function
entry points and HLEs 237 imported firmware NIDs. The NID-to-stub binding table is generated
per game into `output/syscall_table.cpp` from the analysis; the runtime carries no per-game
stub addresses.

## Pipeline

```mermaid
flowchart LR
    A[BOOT.BIN] -->|psprecomp analyze<br/>Ghidra headless + ELF parser| B[analysis.json]
    B -->|psprecomp recompile| C[output/<br/>C++17 source]
    C -->|cmake build| D[psprecomp_runtime]
    E[runtime/<br/>memory, scheduler, HLE, GE] --> D
    F[disc0/<br/>extracted ISO content] --> D
    D -->|SDL2 + OpenGL 3.3| G[Window]
```

## Why Ghidra?

A static recompiler must know where every function begins and ends **before** it decodes
anything — and a retail PSP binary doesn't say. BOOT.BIN is stripped: no symbol table, no
function boundaries, just a flat region of MIPS instructions and data. Simply decoding from the
entry point and following calls is not enough, because much of the code is only reachable
indirectly (C++ vtables, function-pointer tables, thread entry points, callbacks registered with
the OS).

Ghidra solves exactly that one problem. The `analyze` step runs Ghidra's headless auto-analysis
— a mature, battle-tested function-discovery engine — over the binary, and the
[`analysis/ExtractAnalysis.java`](analysis/ExtractAnalysis.java) script exports the results to
`analysis.json`: function entry points and sizes, mid-function entry points, cross-references,
and jump-table hints. For Patapon that census is ~9,700 functions.

Two things follow from this design:

- **Ghidra is needed once per binary.** After `analysis.json` exists, the `recompile` step and
  the runtime never touch Ghidra again. It is an analysis-time tool, not a runtime dependency —
  everything downstream (the decoder, the C++ emitter, the entire runtime) is this project's own
  code.
- **Ghidra's census is treated as a starting point, not ground truth.** Auto-analysis misses
  functions (no inbound xrefs, data-driven dispatch), so the pipeline supplements it: forced
  mid-entry injection and cross-function mid-jump discovery recover targets that surface at
  runtime as dispatch-table misses.

The PSP's CPU (Allegrex) is a MIPS32 variant with custom instructions and a vector unit (VFPU)
that stock Ghidra does not understand, so the
[ghidra-allegrex](https://github.com/kotcrab/ghidra-allegrex) processor extension is required —
see [Prerequisites](#prerequisites-macos--homebrew).

## Current Status

**As of 2026-06-10: the game boots and renders the PATAPON title screen** — logo, NEW
GAME/CONTINUE menu, and copyright text, visually matching PPSSPP. This is the first real graphics
the runtime has displayed; earlier rendering milestones were measured in display-list metrics
only.

![PATAPON title screen rendered by psprecomp_runtime](docs/title-screen.png)

Recent work that got it there (merged via PR #17):

- **Faithful IO HLE** — PPSSPP-exact rejection of NULL/empty `sceIoOpen` paths, a file-descriptor
  cap, and `BADF` errors; fixed a leak of ~159,000 fds during boot.
- **Emitter fix** — the FPU integer and float register views (`f[]`/`fi[]`) now alias via an
  anonymous union; previously every `lwc1`-fed float computation in the binary was a no-op.
- **GE SIGNAL flow control** — display-list JUMP/CALL/RET behaviors (0x10–0x12); Patapon keeps
  all real geometry in SIGNAL-called sub-lists, all of which were previously skipped.
- **Renderer fixes** — CLUT palette addressing (palettes were read from zeroed RAM, making all
  texels transparent) and column-major PSP matrix layout in the vertex transform (vertices
  previously collapsed to a point).

## Building and Running

### Prerequisites (macOS / Homebrew)

```bash
brew install cmake sdl2 pkg-config ghidra
```

- **Rust** (stable) and a **C++17 compiler**; CMake 3.16+, SDL2 (found via pkg-config), OpenGL 3.3.
- **Ghidra 12.x** — used by the `analyze` step only (see [Why Ghidra?](#why-ghidra)).
- **[ghidra-allegrex](https://github.com/kotcrab/ghidra-allegrex)** — Ghidra processor extension
  for the PSP's Allegrex CPU. Install it into your Ghidra (Ghidra GUI: *File → Install
  Extensions*, or unzip into the install dir); the analyze step verifies that
  `<ghidra-install>/Ghidra/Processors/Allegrex` exists and refuses to run without it.

You must also provide, from your own copy of the game (no game data is included in or
distributed with this repository):

- `BOOT.BIN` — the game executable, from `PSP_GAME/SYSDIR/` on the disc.
- `disc0/` — the extracted ISO contents (assets the game loads at runtime).

The `analyze` step also needs the PSP NID database (`data/niddb/ppsspp_niddb.xml`), which
maps firmware function NIDs to names. It is not committed (PPSSPP-derived; `data/` is
gitignored). Fetch it once after cloning:

```bash
./scripts/fetch-niddb.sh
```

It downloads from [pspdev/psp-ghidra-scripts](https://github.com/pspdev/psp-ghidra-scripts)
(NID names derived from PPSSPP, GPL-2.0-or-later), verifies the checksum, and is idempotent.
If `analyze` is run without it, it stops with a message pointing at this script.

### Full pipeline

```bash
# 1. Rust pipeline
cargo build --release
cargo test

# 1b. NID database (once per clone; needed by analyze)
./scripts/fetch-niddb.sh

# 2. Analyze the binary -> analysis.json (requires Ghidra + ghidra-allegrex; once per binary)
cargo run --release -- analyze --ghidra-dir "$(brew --prefix ghidra)/libexec" BOOT.BIN

# 3. Generate C++ source from analysis.json (PSPRECOMP_CROSS_MID=1 is required;
#    --config selects the per-game manifest — Patapon's carries its force entries)
PSPRECOMP_CROSS_MID=1 cargo run --release -- recompile analysis.json \
    --config games/patapon/game.toml -o output

# 4. Build the runtime (PSPRECOMP_GAME selects the games/<id>/ hook module;
#    defaults to "patapon". -DPSPRECOMP_GAME=none builds a pure generic
#    runtime with zero game-specific hooks — see ARCHITECTURE.md "Per-Game Layer")
cmake -B runtime/build -S runtime && cmake --build runtime/build -j$(sysctl -n hw.ncpu)

# 5. Run (the configuration the current status was verified under)
PSPRECOMP_CROSS_MID=1 PSPRECOMP_CLEANROOM=1 ./runtime/build/psprecomp_runtime
```

Useful environment variables:

| Variable | Effect |
|----------|--------|
| `PSPRECOMP_CROSS_MID=1` | Cross-function mid-jump support; set at both recompile and run in the standard workflow |
| `PSPRECOMP_CLEANROOM=1` | Faithful HLE pass-throughs instead of legacy debug wrappers; the standard verification configuration |
| `PSPRECOMP_DISC0=/path` | Extracted ISO content directory (default `./disc0`) |
| `PSPRECOMP_STRICT=1` | Abort on dispatch-table miss (debugging) |

Inspect the emitted C++ for a single function:

```bash
cargo run --release -- dump analysis.json 0xADDRESS
```

## Verification Methodology

PPSSPP is used as a scriptable behavioral oracle (breakpoints, memory reads, and input injection
through its debugger API) to capture ground truth and find the first divergence from our runtime.
The runtime exposes a TCP debug socket on port 9999 for live memory inspection, and changes are
checked with adversarial sub-agent verification before they are banked.

## Limitations

- **Temporary renderer fallbacks.** A known open guest-side bug remains: recompiled FPU/VFPU code
  computes broken view/projection matrices (all-zero view, NaN projection). A renderer fallback
  compensates: degenerate matrices route to a game-installable NDC mapping
  (`games/patapon/runtime/hooks_ge.cpp` provides Patapon's ortho; generic builds pass world
  space through with a warning). This is adequate for the 2D title/menu screens but must be
  fixed before 3D gameplay: the mapping hardcodes Patapon's viewport and has no depth ordering.
- **No audio.** ATRAC and SAS are crude stubs (0 samples decoded, instant end-of-stream).
- **GE gaps.** SIGNAL relative/offset variants (0x13–0x18), lighting, texture matrix, bone/morph
  skinning, bezier surfaces, and block transfers (TRANSFERSTART) are unimplemented.
- **Beyond the title screen is unexplored.** Title-screen interactivity (menu input advancing the
  game's state machine) has not yet been exercised in our runtime.
- **A rare race** in the game's IO worker (a phantom job, roughly 1 in 20 boots) is tripwired but
  not fixed.
- **One game renders, single platform.** Patapon BOOT.BIN is the only title that reaches
  graphics. The runtime core itself is now game-agnostic — all Patapon-specific code lives in
  `games/patapon/` behind compile-time seams, enforced by `runtime/tools/purity_gate.sh`
  (no game literals or symbols in core objects) — and a second commercial binary recompiles,
  links against the generic runtime (`-DPSPRECOMP_GAME=none`), boots through `module_start`,
  and runs its main thread, but produces no graphics yet (bring-up is the next phase).
  Type-B (0x700000A1) packed relocations are detected and rejected with an explicit error.
  Developed and tested on macOS only; Linux and Windows have never been tried (the build
  assumes SDL2 via pkg-config and OpenGL 3.3, and the render-queue threading model was
  designed around macOS constraints). Testing and supporting other operating systems is a
  to-do. Optimizer passes are disabled by design until a later phase.

## Reference Projects

- [N64Recomp](https://github.com/N64Recomp/N64Recomp) — N64 static recompiler with a similar
  dispatch-table/context-struct architecture
- [PPSSPP](https://github.com/hrydgard/ppsspp) — PSP emulator, used as a behavioral oracle
  (observable behavior only)

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).

Portions of the runtime (VFPU register-file semantics, GE command constants, HLE error codes
and kernel-object semantics) are derived from [PPSSPP](https://github.com/hrydgard/ppsspp),
which is licensed GPL-2.0-or-later.

Bundled third-party loaders (`runtime/src/glad.c`, `runtime/include/glad/`,
`runtime/include/KHR/khrplatform.h`) carry their own permissive licenses, noted in their
file headers.
