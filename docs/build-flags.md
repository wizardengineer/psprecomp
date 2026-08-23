# PSPRECOMP build and environment flags

Consolidated `PSPRECOMP_*` flags discovered in-tree so contributors do not have to grep the codebase.

| Flag | Where referenced |
|------|------------------|
| `PSPRECOMP_AEC_TRACE` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_main.cpp` |
| `PSPRECOMP_ALLOW_STALE` | `DEBUGGING.md`, `crates/psp-cli/src/fingerprint.rs`, `docs/ENV_FLAGS.md`, `docs/TROUBLESHOOTING.md`, `runtime/CMakeLists.txt` |
| `PSPRECOMP_ASSETPROC_ZERO` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_main.cpp` |
| `PSPRECOMP_ASSET_FORCE_MISS` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/asset_bnd.cpp`, `games/patapon/runtime/hooks_main.cpp` |
| `PSPRECOMP_ASSET_NO_STUB` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/asset_bnd.cpp`, `games/patapon/runtime/asset_bnd.h`, `games/patapon/runtime/hooks_main.cpp` |
| `PSPRECOMP_ASSET_SIZE_HINT` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/asset_bnd.cpp` |
| `PSPRECOMP_B6C_TRACE` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_main.cpp` |
| `PSPRECOMP_BND_CONTENT_CHECK` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/asset_bnd.cpp` |
| `PSPRECOMP_BND_DISABLE` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_main.cpp`, `games/patapon/scripts/test_phase11_1_no_retry.sh`, `games/patapon/scripts/test_phase11_2_gatekeeper.sh` |
| `PSPRECOMP_BND_SHORT_OFF` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_main.cpp` |
| `PSPRECOMP_BND_TRACE` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/asset_bnd.cpp` |
| `PSPRECOMP_CLEANROOM` | `DEBUGGING.md`, `README.md`, `docs/ADDING_A_GAME.md`, `docs/ENV_FLAGS.md`, `docs/SCHEDULER-DESIGN.md` |
| `PSPRECOMP_COALESCE_DEBUG` | `crates/psp-cli/src/recompile.rs`, `docs/ENV_FLAGS.md` |
| `PSPRECOMP_CROSS_MID` | `ARCHITECTURE.md`, `CLAUDE.md`, `DEBUGGING.md`, `README.md`, `crates/psp-cli/src/config.rs` |
| `PSPRECOMP_CTX_PROBE` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_memory.cpp` |
| `PSPRECOMP_D1_TRACE` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_memory.cpp` |
| `PSPRECOMP_DF0_PROBE` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_memory.cpp` |
| `PSPRECOMP_DIAG07` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_main.cpp` |
| `PSPRECOMP_DISC0` | `DEBUGGING.md`, `README.md`, `docs/ADDING_A_GAME.md`, `docs/ENV_FLAGS.md`, `docs/SCHEDULER-DESIGN.md` |
| `PSPRECOMP_E28_TRACE` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_main.cpp` |
| `PSPRECOMP_END_PROBE` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_memory.cpp` |
| `PSPRECOMP_FE90_OBJ` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_memory.cpp` |
| `PSPRECOMP_FFC_ZERO` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_main.cpp` |
| `PSPRECOMP_FP_ERROR` | `docs/ENV_FLAGS.md`, `runtime/CMakeLists.txt` |
| `PSPRECOMP_FP_OUTPUT` | `docs/ENV_FLAGS.md`, `runtime/CMakeLists.txt` |
| `PSPRECOMP_FP_RESULT` | `docs/ENV_FLAGS.md`, `runtime/CMakeLists.txt` |
| `PSPRECOMP_GAME` | `ARCHITECTURE.md`, `CLAUDE.md`, `DEBUGGING.md`, `README.md`, `crates/psp-cli/src/config.rs` |
| `PSPRECOMP_GATE_PROBE` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_main.cpp` |
| `PSPRECOMP_GEOM_SELFTEST` | `docs/ENV_FLAGS.md`, `docs/GRAPHICS.md`, `runtime/src/psp_ge_draw.cpp` |
| `PSPRECOMP_GE_TEST` | `docs/ENV_FLAGS.md`, `docs/GRAPHICS.md`, `games/patapon/runtime/hooks_main.cpp`, `games/patapon/scripts/test_phase11_1_no_retry.sh`, `games/patapon/scripts/test_phase11_2_gatekeeper.sh` |
| `PSPRECOMP_GE_TEST_ONLY` | `docs/ENV_FLAGS.md`, `docs/GRAPHICS.md`, `games/patapon/runtime/hooks_main.cpp`, `games/patapon/scripts/test_phase11_1_no_retry.sh`, `games/patapon/scripts/test_phase11_2_gatekeeper.sh` |
| `PSPRECOMP_GE_TRACE` | `DEBUGGING.md`, `docs/ENV_FLAGS.md`, `docs/GRAPHICS.md`, `runtime/src/psp_ge.cpp` |
| `PSPRECOMP_GK_BUF` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_main.cpp` |
| `PSPRECOMP_GK_ZERO` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_main.cpp` |
| `PSPRECOMP_HLE_TRACE` | `DEBUGGING.md`, `docs/ENV_FLAGS.md`, `runtime/include/hle/psp_hle.h`, `runtime/src/hle/psp_hle_dispatch.cpp`, `runtime/src/hle/psp_hle_kernel_sema.cpp` |
| `PSPRECOMP_ITER_PROBE` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_memory.cpp` |
| `PSPRECOMP_LISTLOOP_OFF` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_memory.cpp` |
| `PSPRECOMP_LK_FIX` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_main.cpp` |
| `PSPRECOMP_LK_KEY` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_main.cpp` |
| `PSPRECOMP_LK_TRACE` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_main.cpp` |
| `PSPRECOMP_LK_ZERO` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_main.cpp` |
| `PSPRECOMP_LK_ZHI` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_main.cpp` |
| `PSPRECOMP_LK_ZLO` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_main.cpp` |
| `PSPRECOMP_LOOSE_GROUP_ABSENT_OFF` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_main.cpp` |
| `PSPRECOMP_LOOSE_REGION_FIX` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_main.cpp` |
| `PSPRECOMP_LOOSE_REGION_FIX_OFF` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_main.cpp` |
| `PSPRECOMP_LOOSE_REGION_SEED` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_memory.cpp` |
| `PSPRECOMP_NO_COALESCE` | `DEBUGGING.md`, `crates/psp-cli/src/recompile.rs`, `docs/ENV_FLAGS.md` |
| `PSPRECOMP_NO_DESC_XLATE` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_main.cpp` |
| `PSPRECOMP_NO_RA_MODEL` | `DEBUGGING.md`, `crates/psp-cli/src/recompile.rs`, `docs/ENV_FLAGS.md` |
| `PSPRECOMP_O980_TRACE` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_main.cpp` |
| `PSPRECOMP_OBJ4_FIX_OFF` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_main.cpp` |
| `PSPRECOMP_OUTPUT_DIR` | `ARCHITECTURE.md`, `docs/ENV_FLAGS.md`, `runtime/CMakeLists.txt` |
| `PSPRECOMP_PC_TRACE` | `DEBUGGING.md`, `crates/psp-emitter/src/cpp_generator.rs`, `docs/ENV_FLAGS.md`, `runtime/src/psp_dispatch.cpp` |
| `PSPRECOMP_PREEMPT` | `crates/psp-emitter/src/cpp_generator.rs`, `crates/psp-emitter/src/function.rs`, `docs/ENV_FLAGS.md`, `docs/SCHEDULER-DESIGN.md`, `runtime/include/psp_scheduler.h` |
| `PSPRECOMP_PRESENT_STALE_MS` | `docs/ENV_FLAGS.md`, `runtime/include/psp_ge_draw.h`, `runtime/src/psp_ge_draw.cpp` |
| `PSPRECOMP_PUSH_TRACE` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_memory.cpp` |
| `PSPRECOMP_REGEN_CMD` | `docs/ENV_FLAGS.md`, `runtime/CMakeLists.txt` |
| `PSPRECOMP_REGION_BUF_FIX` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_main.cpp` |
| `PSPRECOMP_REGION_BUF_FIX_OFF` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_main.cpp` |
| `PSPRECOMP_RUN_CTORS` | `docs/ENV_FLAGS.md`, `games/patapon/runtime/hooks_main.cpp`, `runtime/src/main.cpp` |
| `PSPRECOMP_SCREENSHOT` | `DEBUGGING.md`, `docs/ENV_FLAGS.md`, `docs/GRAPHICS.md`, `runtime/src/psp_ge_draw.cpp` |
| `PSPRECOMP_SEMA_TRACE` | `DEBUGGING.md`, `docs/ENV_FLAGS.md`, `runtime/src/hle/psp_hle_kernel_sema.cpp` |
| `PSPRECOMP_SPLEAK` | `DEBUGGING.md`, `docs/ENV_FLAGS.md`, `runtime/src/psp_dispatch.cpp` |
| `PSPRECOMP_STRICT` | `ARCHITECTURE.md`, `CLAUDE.md`, `DEBUGGING.md`, `README.md`, `docs/ADDING_A_GAME.md` |

Default values and semantics live next to the definitions; prefer those sources of truth when changing behavior.
