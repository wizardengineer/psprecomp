#pragma once
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <unordered_map>
#include <unordered_set>

// Forward declarations
struct recomp_context;
using FuncPtr = void(*)(uint8_t*, recomp_context*);

// HLE function pointer type
using HleFunc = void(*)(uint8_t*, recomp_context*);

// ---- PSP Error Codes ----
// Standard PSP kernel error codes (from PSP SDK / PPSSPP)
constexpr int32_t SCE_OK = 0;

// Errno errors
constexpr int32_t SCE_ERROR_ERRNO_ENOENT        = (int32_t)0x80010002U;
constexpr int32_t SCE_ERROR_ERRNO_EIO            = (int32_t)0x80010005U;
constexpr int32_t SCE_ERROR_ERRNO_ENOMEM         = (int32_t)0x8001000CU;
constexpr int32_t SCE_ERROR_ERRNO_EACCES         = (int32_t)0x8001000DU;
constexpr int32_t SCE_ERROR_ERRNO_EEXIST         = (int32_t)0x80010011U;
constexpr int32_t SCE_ERROR_ERRNO_ENODEV         = (int32_t)0x80010013U;
constexpr int32_t SCE_ERROR_ERRNO_EINVAL         = (int32_t)0x80010016U;
constexpr int32_t SCE_ERROR_ERRNO_EMFILE         = (int32_t)0x80010018U;
constexpr int32_t SCE_ERROR_ERRNO_ENOSPC         = (int32_t)0x8001001CU;

// Kernel errors
constexpr int32_t SCE_KERNEL_ERROR_OK            = 0;
constexpr int32_t SCE_KERNEL_ERROR_ERROR         = (int32_t)0x80020001U;
constexpr int32_t SCE_KERNEL_ERROR_NO_MEMORY     = (int32_t)0x800200D8U;
constexpr int32_t SCE_KERNEL_ERROR_ILLEGAL_ATTR  = (int32_t)0x800200CBU;
constexpr int32_t SCE_KERNEL_ERROR_ILLEGAL_THREAD  = (int32_t)0x800201BDU;
constexpr int32_t SCE_KERNEL_ERROR_NOT_FOUND_THREAD = (int32_t)0x800201BEU;
constexpr int32_t SCE_KERNEL_ERROR_WAIT_TIMEOUT  = (int32_t)0x800201A8U;
constexpr int32_t SCE_KERNEL_ERROR_SEMA_ZERO     = (int32_t)0x800201AEU;
constexpr int32_t SCE_KERNEL_ERROR_SEMA_OVERFLOW = (int32_t)0x800201AFU;
constexpr int32_t SCE_KERNEL_ERROR_MUTEX_NOT_FOUND = (int32_t)0x800201C3U;
constexpr int32_t SCE_KERNEL_ERROR_EVF_NOT_FOUND = (int32_t)0x800201BFU;
constexpr int32_t SCE_KERNEL_ERROR_NOT_FOUND_MODULE = (int32_t)0x80020196U;
// MsgPipe errors (PPSSPP Core/HLE/ErrorCodes.h; match pspkerror.h)
constexpr int32_t SCE_KERNEL_ERROR_UNKNOWN_MPPID = (int32_t)0x8002019EU;
constexpr int32_t SCE_KERNEL_ERROR_MPP_FULL      = (int32_t)0x800201B3U;
constexpr int32_t SCE_KERNEL_ERROR_MPP_EMPTY     = (int32_t)0x800201B4U;
constexpr int32_t SCE_KERNEL_ERROR_ILLEGAL_SIZE  = (int32_t)0x800201BCU;
// IO kernel errors (PPSSPP Core/HLE/sceIo.cpp). NOTE: distinct from the
// errno-style SCE_ERROR_ERRNO_EMFILE (0x80010018) above.
constexpr int32_t SCE_KERNEL_ERROR_MFILE         = (int32_t)0x80020320U;
constexpr int32_t SCE_KERNEL_ERROR_BADF          = (int32_t)0x80020323U;

// ---- PSP Memory Constants ----
// PSP_ADDR_MASK (0x07FFFFFFU) is defined in psp_memory.h
constexpr uint32_t PSP_USER_MEM_BASE  = 0x08800000U;
constexpr uint32_t PSP_USER_MEM_END   = 0x0C000000U;  // PSP Slim (64MB): user space up to 0x0C000000
// BND parser arena — host-side asset staging region OUTSIDE guest user memory.
//
// History: the arena originally lived at 0x0B000000..0x0C000000 (carved from
// the top of user memory, Phase 11 D-02). That collided with guest thread
// stacks, which `psp_alloc_stack` carves DOWN from PSP_USER_MEM_END - 0x1000
// = 0x0BFFF000: by the time titledata.bnd (1.4 MB decompressed) resolved, the
// arena bump cursor had reached ~0x0BE3CC00 and the payload memcpy stomped
// user_main's live stack frames (saved s3 = engine base overwritten →
// frame-tick list walk never terminated → boot halted at Frame 5). The arena
// also OOMed at 16 MB.
//
// New placement: guest VA 0x0C400000..0x0F400000 (48 MB). Alias safety under
// the runtime's 0x07FFFFFFU mask into the 128 MB rdram:
//   - arena masks to rdram [0x04400000, 0x07400000)
//   - guest user RAM 0x08000000..0x0BFFFFFF masks to [0x00000000, 0x04000000)
//   - VRAM 0x04000000..0x041FFFFF (and uncached 0x44000000 alias) masks to
//     [0x04000000, 0x04200000) — arena starts 2 MB above it
//   - scratchpad 0x00010000 masks to itself (far below)
//   - the [S252] probe stack at 0x0FF00000 masks to 0x07F00000 — above the
//     arena end with 11 MB margin
// No guest region or runtime reservation aliases into [0x04400000, 0x07400000).
constexpr uint32_t PSP_BND_ARENA_BASE = 0x0C400000U;
constexpr uint32_t PSP_BND_ARENA_END  = 0x0F400000U;
constexpr uint32_t PSP_KERNEL_MEM_BASE = 0x08000000U;

// Compile-time alias-safety guards for the BND arena (mask = 0x07FFFFFFU,
// rdram = 128 MB = 0x08000000 bytes, VRAM masked end = 0x04200000).
static_assert(PSP_BND_ARENA_BASE >= PSP_USER_MEM_END,
              "BND arena must not overlap guest user memory / thread stacks");
static_assert((PSP_BND_ARENA_BASE & 0x07FFFFFFU) >= 0x04200000U,
              "BND arena (masked) must not overlap VRAM");
static_assert(((PSP_BND_ARENA_END - 1U) & 0x07FFFFFFU) < 0x08000000U
                  && (PSP_BND_ARENA_END & 0x07FFFFFFU)
                         > (PSP_BND_ARENA_BASE & 0x07FFFFFFU),
              "BND arena (masked) must fit contiguously inside 128MB rdram");

// ---- Kernel Memory Allocator ----

/// Allocate memory in the PSP kernel memory region (0x08000000-0x083FFFFF).
/// Simple bump allocator for NativeModule structs and similar kernel objects.
/// Returns PSP virtual address (not masked). Returns 0 on OOM.
uint32_t psp_alloc_kernel_memory(uint32_t size);

// ---- Boot Module Constants ----

/// Boot module UID (assigned during init, returned by sceKernelGetModuleId)
constexpr int BOOT_MODULE_UID = 1;
/// Boot module NativeModule address in kernel memory
constexpr uint32_t BOOT_MODULE_ADDR = 0x08000100U;
/// NativeModule struct size (from PPSSPP sceKernelModule.h)
constexpr uint32_t NATIVE_MODULE_SIZE = 0xC4U;  // 196 bytes
/// Get the boot module's GP value
uint32_t psp_get_boot_module_gp();

// ---- HLE Trace Control ----

/// When true, every HLE function call logs thread name and function name
/// to stderr. Controlled by PSPRECOMP_HLE_TRACE=1 environment variable.
/// Initialized during psp_hle_init().
extern bool g_hle_trace_enabled;

// ---- HLE Dispatch API ----

/// Register an HLE function for a specific NID name.
/// Called during init by each per-module registration function.
void psp_hle_register(const char* nid_name, HleFunc fn);

/// Initialize HLE subsystem: register all module stubs,
/// then override dispatch table entries for the 237 import stubs.
void psp_hle_init();

/// Central HLE syscall dispatcher (for actual syscall instructions in binary).
/// Called from generated code for MipsOp::Syscall instructions.
void psp_hle_syscall(uint8_t* rdram, recomp_context* ctx, uint32_t code);

/// Override a dispatch table entry (provided by generated dispatch.cpp).
extern void psp_dispatch_register(uint32_t vaddr, FuncPtr fn);

/// Override the game's dlmalloc allocator (FUN_0881E558, FUN_0881E7A8)
/// with a native bump allocator. Must be called after psp_hle_init().
void psp_dlmalloc_override_init();

/// Override the game's CRT memory functions (memmove, memcpy, memset)
/// with native implementations. The recompiled MIPS versions use
/// LWL/LWR/SWL/SWR (unaligned access) which are emitted as no-op stubs,
/// causing infinite loops or data corruption in copy prologues.
void psp_crt_override_init();

/// Override the game's CRT libc assertion handler (FUN_088133EC) that
/// prints "no reent structure found" and calls sceKernelExitThread(1).
/// The override returns gracefully instead of killing the thread,
/// allowing the game to handle allocation failures without crashing.
void psp_crt_assertion_override_init();

// ---- Per-Module Registration Functions ----
// Each HLE module file provides a registration function.
// Called by psp_hle_register_all_modules() during init.
void psp_hle_register_all_modules();

// Kernel modules (04-02, 04-03)
void psp_hle_register_kernel_thread();
void psp_hle_register_kernel_memory();
void psp_hle_register_kernel_sema();
void psp_hle_register_kernel_mutex();
void psp_hle_register_kernel_eventflag();

// I/O module (04-04)
void psp_hle_register_io();

// Display, GE, power, ctrl, utility (04-05)
void psp_hle_register_display();
void psp_hle_register_ge();
void psp_hle_register_power();
void psp_hle_register_ctrl();
void psp_hle_register_utility();

// SAS voice state machine (issue #29)
void psp_hle_register_sas();

// ---- Phase 11.2 wrapper consolidation (Pattern F / FORM 2) ----
// Forward declaration of the GE-gatekeeper fixup body originally defined in
// runtime/src/hle/psp_hle_kernel_memory.cpp. The function performs the
// GE_BASE_FIX (reconstructs r4 from RQ_GLOBAL_ADDR) + GE_GATE_FIX
// (reconstructs r6/r8/r10 from g_current_obj_sm) and tail-calls
// FUN_088623e0(rdram, ctx). Phase 11.2 moves the dispatch registration into
// runtime/src/main.cpp's consolidated gatekeeper wrapper (Pattern F section
// (a)+(b)+(e) — see main.cpp:165-207). The function body is retained as
// callable code (NOT dead code) and invoked from the consolidated lambda.
void hle_debug_088623E0(uint8_t* rdram, recomp_context* ctx);
