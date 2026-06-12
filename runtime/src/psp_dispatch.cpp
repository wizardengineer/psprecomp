#include "recomp.h"
#include "hle/psp_hle.h"
#include "hle/psp_hle_kernel.h"
#include "psp_scheduler.h"
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <algorithm>
#include <execinfo.h>
#include <dlfcn.h>

/// Cached STRICT mode flag — checked once on first call.
static bool g_strict_mode = false;
static bool g_strict_checked = false;

/// Per-address invocation counter for LOOKUP_MISS diagnostics.
/// Logs each unique address on first hit only to reduce noise.
static std::unordered_map<uint32_t, int> g_miss_counts;

/// Last function address from psp_trace_checkpoint (defined below, always recorded).
thread_local uint32_t g_last_func_addr = 0;

/// No-op stub returned when a lookup miss occurs in non-STRICT mode.
/// Sets v0 (ctx->r[2]) to 0 for deterministic return value behavior.
static thread_local uint32_t g_last_miss_addr = 0;

// [V438] ring buffer of recent function entries (poor-man's backtrace).
// Declared here so noop_stub (below) can read it; written by
// psp_trace_checkpoint. Cheap: one array write per checkpoint.
thread_local uint32_t g_func_ring[32] = {0};
thread_local uint32_t g_func_ring_pos = 0;

// [#35] Cross-thread copy of the dispatched-function ring for the debug
// socket's I command. The per-thread g_func_ring above is thread_local and
// unreadable from the socket thread, so psp_trace_checkpoint also appends
// to this shared ring with relaxed atomics (interleaves all game threads;
// ordering across threads is approximate -- diagnostics only).
static std::atomic<uint32_t> g_shared_func_ring[64];
static std::atomic<uint32_t> g_shared_func_ring_pos{0};

// [#35] LOOKUP_MISS counters readable from the debug socket thread.
// The g_miss_counts map below is mutated without a lock from game threads,
// so the socket must NOT iterate it (rehash mid-read). These atomics carry
// the two numbers the I command needs.
static std::atomic<uint32_t> g_miss_unique{0};
static std::atomic<uint64_t> g_miss_total{0};

void psp_dispatch_get_miss_stats(uint32_t* unique_addrs,
                                 uint64_t* total_calls) {
    if (unique_addrs)
        *unique_addrs = g_miss_unique.load(std::memory_order_relaxed);
    if (total_calls)
        *total_calls = g_miss_total.load(std::memory_order_relaxed);
}

int psp_dispatch_get_recent_funcs(uint32_t* out, int max) {
    uint32_t pos = g_shared_func_ring_pos.load(std::memory_order_relaxed);
    int n = 0;
    // Oldest first: walk forward from the slot the next write would claim.
    for (int i = 0; i < 64 && n < max; i++) {
        uint32_t v = g_shared_func_ring[(pos + static_cast<uint32_t>(i)) & 63u]
                         .load(std::memory_order_relaxed);
        if (v) out[n++] = v;
    }
    return n;
}

static void noop_stub(uint8_t* rdram, recomp_context* ctx) {
    uint32_t addr = g_last_miss_addr;
    int& c = g_miss_counts[addr];
    if (c <= 5) {
        std::fprintf(stderr,
            "[LOOKUP_MISS_CTX] addr=0x%08X caller=0x%08X a0=0x%08X a1=0x%08X sp=0x%08X"
            " r16=0x%08X r17=0x%08X r21=0x%08X\n",
            addr,
            g_last_func_addr,
            static_cast<uint32_t>(ctx->r[4]),
            static_cast<uint32_t>(ctx->r[5]),
            static_cast<uint32_t>(ctx->r[29]),
            static_cast<uint32_t>(ctx->r[16]),
            static_cast<uint32_t>(ctx->r[17]),
            static_cast<uint32_t>(ctx->r[21]));
        // If r17 looks like a valid PSP slot ptr, dump slot fields
        uint32_t r17 = static_cast<uint32_t>(ctx->r[17]);
        if (r17 >= 0x08000000U && r17 < 0x0A000000U) {
            uint32_t base = r17 & 0x07FFFFFFU;
            auto rd32 = [&](uint32_t off) {
                return *reinterpret_cast<uint32_t*>(rdram + base + off);
            };
            std::fprintf(stderr,
                "[LOOKUP_MISS_CTX]   slot: state=%u ap=%u fd=%u [292]=%u [300]=%u [308]=%u\n",
                rd32(0), rd32(12), rd32(16), rd32(292), rd32(300), rd32(308));
        }
    }

    // [BND_VTABLE_MISS] — if g_last_func_addr is in the BND arena, the caller
    // was a noop_stub-filled vtable slot allocated by Phase 11's
    // bnd_resolve_and_allocate (allocate_shared_noop_vtable). Punch-list
    // output for Phase 11.2 R11.2-09 + A12 acceptance — fires regardless of
    // the per-address `c <= 5` quota above so we always see new vtable
    // sources as the consumer advances. See Pattern E in 11.2-PATTERNS.md.
    if (g_last_func_addr >= PSP_BND_ARENA_BASE
            && g_last_func_addr < PSP_BND_ARENA_END) {
        static int vtable_miss_count = 0;
        vtable_miss_count++;
        if (vtable_miss_count <= 8 || vtable_miss_count % 500 == 0) {
            std::fprintf(stderr,
                "[BND_VTABLE_MISS] from=0x%08X (BND arena vtable entry) "
                "target=0x%08X (#%d)\n",
                g_last_func_addr, addr, vtable_miss_count);
        }
    }

    // (scan code removed — target address found: 0x09012CD4)

    // WORKAROUND: 0x438 is a corrupt vtable dispatch that should call
    // FUN_0895b798 (sceKernelSignalSema thunk for uid=259).
    // The original function loads the UID from the object, but the object
    // pointer is corrupt (0x0003796C — invalid PSP address). Signal uid=259
    // directly to keep the render pipeline flowing.
    if (addr == 0x438) {
        // [V438] Native-path root-cause probe (Phase 12): capture the REAL
        // indirect-call site. In recompiled MIPS, jalr sets r31=return addr,
        // so ctx->r[31] points just past the bad call. a0..a3 carry the
        // object/args. Dump the object's first words to see where the
        // corrupt pointer (0x0003796C = asset size) originates.
        static int v438_count = 0;
        if (++v438_count <= 8) {
            // Host return address → the generated FUN_ that made this call.
            void* host_ra = __builtin_return_address(0);
            Dl_info dli; const char* sym = "?";
            if (dladdr(host_ra, &dli) && dli.dli_sname) sym = dli.dli_sname;
            std::fprintf(stderr, "[V438] host_ra=%p sym=%s r25=0x%08X\n",
                host_ra, sym, static_cast<uint32_t>(ctx->r[25]));
            uint32_t a0 = static_cast<uint32_t>(ctx->r[4]);
            std::fprintf(stderr,
                "[V438] #%d ra=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X "
                "s0=0x%08X s1=0x%08X s2=0x%08X s3=0x%08X gp=0x%08X\n",
                v438_count,
                static_cast<uint32_t>(ctx->r[31]),
                a0, static_cast<uint32_t>(ctx->r[5]),
                static_cast<uint32_t>(ctx->r[6]),
                static_cast<uint32_t>(ctx->r[16]),
                static_cast<uint32_t>(ctx->r[17]),
                static_cast<uint32_t>(ctx->r[18]),
                static_cast<uint32_t>(ctx->r[19]),
                static_cast<uint32_t>(ctx->r[28]));
            if (a0 >= 0x08000000U && a0 < 0x0A000000U) {
                uint32_t b = a0 & 0x07FFFFFFU;
                std::fprintf(stderr,
                    "[V438]   *a0[0..3]=0x%08X 0x%08X 0x%08X 0x%08X\n",
                    *reinterpret_cast<uint32_t*>(rdram + b + 0),
                    *reinterpret_cast<uint32_t*>(rdram + b + 4),
                    *reinterpret_cast<uint32_t*>(rdram + b + 8),
                    *reinterpret_cast<uint32_t*>(rdram + b + 12));
            }
            // Recent function-entry chain (most recent last).
            std::fprintf(stderr, "[V438]   ring:");
            for (int i = 0; i < 32; ++i) {
                uint32_t e = g_func_ring[(g_func_ring_pos + i) & 31u];
                if (e) std::fprintf(stderr, " %08X", e);
            }
            std::fprintf(stderr, "\n");
            // Dump outer object s0=0x089F8710 fields 0..96 to find the NULL sub-ptr.
            uint32_t s0 = static_cast<uint32_t>(ctx->r[16]);
            if (s0 >= 0x08000000U && s0 < 0x0A000000U) {
                uint32_t b = s0 & 0x07FFFFFFU;
                std::fprintf(stderr, "[V438]   s0obj@0x%08X:", s0);
                for (uint32_t o = 0; o <= 96; o += 4) {
                    std::fprintf(stderr, " +%u=%08X", o,
                        *reinterpret_cast<uint32_t*>(rdram + b + o));
                }
                std::fprintf(stderr, "\n");
            }
            // [V438FULL] full register file + dump *(reg) for any reg that looks
            // like a live PSP object pointer, to find the bogus object register
            // (same-snapshot; addresses captured at OTHER times are unreliable).
            // [V438BT] real host backtrace — names the recompiled FUN_ call chain
            // at the crash (lldb can't reach this crash point in batch time).
            {
                void* bt[24];
                int n = backtrace(bt, 24);
                char** syms = backtrace_symbols(bt, n);
                if (syms) {
                    std::fprintf(stderr, "[V438BT] host stack (%d frames):\n", n);
                    for (int bi = 0; bi < n; ++bi)
                        std::fprintf(stderr, "[V438BT]   %s\n", syms[bi]);
                    free(syms);
                }
            }
            std::fprintf(stderr, "[V438FULL] regs:");
            for (int ri = 0; ri < 32; ++ri) {
                std::fprintf(stderr, " r%d=%08X", ri,
                    static_cast<uint32_t>(ctx->r[ri]));
            }
            std::fprintf(stderr, "\n");
            for (int ri = 0; ri < 32; ++ri) {
                uint32_t rv = static_cast<uint32_t>(ctx->r[ri]);
                if (rv >= 0x08000000U && rv < 0x0A000000U) {
                    uint32_t rb = rv & 0x07FFFFFFU;
                    std::fprintf(stderr,
                        "[V438FULL]   *r%d@%08X: +0=%08X +4=%08X +8=%08X +12=%08X\n",
                        ri, rv,
                        *reinterpret_cast<uint32_t*>(rdram + rb + 0),
                        *reinterpret_cast<uint32_t*>(rdram + rb + 4),
                        *reinterpret_cast<uint32_t*>(rdram + rb + 8),
                        *reinterpret_cast<uint32_t*>(rdram + rb + 12));
                }
            }
        }
        psp_hle_signal_sema_by_uid(259, 1);
    }


    ctx->r[2] = 0;
}
static bool g_miss_atexit_registered = false;

/// Dump all LOOKUP_MISS addresses sorted by count at program exit.
/// Output format is suitable for feeding back into Ghidra analysis
/// as "force function start" hints for future dispatch table expansion.
static void psp_dump_lookup_misses() {
    if (g_miss_counts.empty()) return;

    // Sort by count descending
    std::vector<std::pair<uint32_t, int>> sorted(
        g_miss_counts.begin(), g_miss_counts.end());
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) {
            return a.second > b.second;
        });

    int total_misses = 0;
    for (const auto& [addr, count] : sorted) {
        total_misses += count;
    }

    std::fprintf(stderr,
        "\n[LOOKUP_MISS_SUMMARY] %zu unique addresses, "
        "%d total calls\n",
        sorted.size(),
        total_misses);

    for (const auto& [addr, count] : sorted) {
        std::fprintf(stderr,
            "  0x%08X (count=%d)\n", addr, count);
    }

    std::fprintf(stderr,
        "[LOOKUP_MISS_SUMMARY] END (%zu addresses)\n",
        sorted.size());
}

/// Called by RECOMP_LOOKUP (in dispatch.cpp) when an address is not in the
/// dispatch table. Logs each unique miss on first hit and returns a noop
/// stub or aborts if PSPRECOMP_STRICT=1.
FuncPtr psp_on_lookup_miss(uint32_t vaddr) {
    // One-time check for STRICT mode environment variable
    if (!g_strict_checked) {
        const char* env = std::getenv("PSPRECOMP_STRICT");
        g_strict_mode = (env && env[0] == '1');
        g_strict_checked = true;
    }

    // Register atexit handler on first miss
    if (!g_miss_atexit_registered) {
        g_miss_atexit_registered = true;
        std::atexit(psp_dump_lookup_misses);
    }

    // Per-address miss counting -- log on first hit only
    int& count = g_miss_counts[vaddr];
    count++;
    g_miss_total.fetch_add(1, std::memory_order_relaxed);
    if (count == 1) {
        g_miss_unique.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr,
            "[LOOKUP_MISS] addr=0x%08X (first hit)\n", vaddr);
    }

    // Store for noop_stub context logging
    g_last_miss_addr = vaddr;

    // STRICT mode: abort on any lookup miss (RUNTIME-11)
    if (g_strict_mode) {
        std::fprintf(stderr,
            "STRICT: aborting on LOOKUP_MISS 0x%08X\n", vaddr);
        std::abort();
    }

    return noop_stub;
}

// Forward declaration for atexit registration
static void psp_dump_pc_trace();

/// PC tracing checkpoint — called at every generated function entry.
/// When PSPRECOMP_PC_TRACE=1, logs function address with frequency counting.
/// No-op when env var is absent or not "1" (single branch cost).
static bool g_pc_trace = false;
static bool g_pc_trace_checked = false;
static std::unordered_map<uint32_t, int> g_func_counts;
static std::mutex g_pc_trace_mutex;
static int g_total_entries = 0;

// Corruption detector: when FUN_0885fe90 is entered with an invalid
// "this" pointer (r[4] < 0x08000000 and non-zero), log the caller chain.
// The corrupt pointer comes from FUN_0885efc8's linked list iteration,
// so g_last_func_addr will show the iterator function.
thread_local uint32_t g_prev_func_addr = 0;

// [SPLEAK] shadow-stack sp-leak detector. Reconstructs sp nesting from function
// entries (no exit hook needed). Reports the function that returned with sp
// imbalanced by >= 0x40. Gated by PSPRECOMP_SPLEAK; default-silent.
struct SpFrame { uint32_t func; uint32_t entry_sp; };
static thread_local std::vector<SpFrame> g_sp_shadow;
static thread_local bool g_spleak_on = false;
static thread_local bool g_spleak_checked = false;

void psp_spleak_checkpoint(uint32_t addr) {
    if (!g_spleak_checked) {
        const char* e = std::getenv("PSPRECOMP_SPLEAK");
        g_spleak_on = (e && e[0] == '1');
        g_spleak_checked = true;
    }
    if (!g_spleak_on) return;
    PspThread* t = psp_get_current_thread();
    if (!t) return;
    uint32_t sp = static_cast<uint32_t>(t->ctx.r[29]);
    // Record a rolling per-visit trace of every function entry + sp during each
    // FE90 (obj 0x0913E900) state-3 visit. When a visit's sp ends up >= +0x380
    // over its baseline (the +896 leak), dump that visit's full trace so the
    // exact entry sequence around the leak is captured.
    struct Ent { uint32_t func; uint32_t sp; };
    static thread_local bool armed = false;
    static thread_local uint32_t base_sp = 0;
    static thread_local std::vector<Ent> buf;
    static thread_local bool dumped = false;
    if (dumped) return;
    if (addr == 0x0885FE90u
            && static_cast<uint32_t>(t->ctx.r[4]) == 0x0913E900u) {
        uint8_t* rd = t->rdram;
        uint32_t st = rd ? *reinterpret_cast<uint32_t*>(
            rd + ((0x0913E900u + 172u) & 0x07FFFFFFu)) : 0;
        if (st == 3) { armed = true; base_sp = sp; buf.clear(); }
        else { armed = false; }
        return;
    }
    if (!armed) return;
    buf.push_back(Ent{addr, sp});
    int32_t over = static_cast<int32_t>(sp - base_sp);
    if (over >= 0x380 && buf.size() > 2) {
        std::fprintf(stderr, "[SPRAW] LEAKING FE90 visit base=0x%08X reached "
                     "over=+%d after %zu entries; dump:\n", base_sp, over, buf.size());
        size_t start = buf.size() > 400 ? buf.size() - 400 : 0;
        uint32_t prev = start ? buf[start-1].sp : base_sp;
        for (size_t i = start; i < buf.size(); i++) {
            int32_t d = static_cast<int32_t>(buf[i].sp - prev);
            std::fprintf(stderr, "[SPRAW] %4zu func=0x%08X sp=0x%08X over=%+d step=%+d\n",
                         i, buf[i].func, buf[i].sp,
                         static_cast<int32_t>(buf[i].sp - base_sp), d);
            prev = buf[i].sp;
        }
        dumped = true; armed = false;
    }
    if (buf.size() > 200000) { armed = false; buf.clear(); }
}

void psp_trace_checkpoint(uint32_t addr) {
    if (!g_pc_trace_checked) {
        const char* env = std::getenv("PSPRECOMP_PC_TRACE");
        g_pc_trace = (env && env[0] == '1');
        g_pc_trace_checked = true;
        if (g_pc_trace) {
            std::atexit(psp_dump_pc_trace);
        }
    }
    g_prev_func_addr = g_last_func_addr;
    g_last_func_addr = addr;
    g_func_ring[(g_func_ring_pos++) & 31u] = addr;
    // [#35] shared (cross-thread) ring for the debug socket I command.
    // One relaxed fetch_add + store per function entry; measured noise is
    // acceptable for a diagnostics-first runtime.
    g_shared_func_ring[g_shared_func_ring_pos.fetch_add(
        1, std::memory_order_relaxed) & 63u]
        .store(addr, std::memory_order_relaxed);

    // [SPLEAK] env PSPRECOMP_SPLEAK: shadow-stack reconstruction of sp at every
    // function entry to pin the function that RETURNS with sp imbalanced (the
    // +0x380 leak on the FE90 completion chain). psp_trace_checkpoint is the
    // FIRST statement of every recompiled function, before its prologue runs,
    // so the observed sp == the caller's sp at the call site. Stack grows down:
    // a normal nested call has entry_sp <= caller's entry_sp; on return sp rises.
    // When entering F at sp_new, every shadow frame with entry_sp < sp_new has
    // returned. If a returned frame's predecessor resumes at a DIFFERENT sp than
    // it was at when it made the call, that predecessor's callee leaked.
    psp_spleak_checkpoint(addr);
    if (!g_pc_trace) return;
    g_total_entries++;

    std::lock_guard<std::mutex> lock(g_pc_trace_mutex);
    int& c = g_func_counts[addr];
    c++;
    if (c <= 3 || c % 100000 == 0) {
        std::fprintf(stderr,
            "[PC-TRACE] func=0x%08X count=%d\n", addr, c);
    }
}

/// Dump the most-called functions sorted by count.
/// Called from a timer thread after a few seconds.
static void psp_dump_pc_trace() {
    std::lock_guard<std::mutex> lock(g_pc_trace_mutex);
    std::fprintf(stderr,
        "\n[PC-TRACE] === DUMP (total entries=%d, last=0x%08X) ===\n",
        g_total_entries, g_last_func_addr);

    // Sort by count descending
    std::vector<std::pair<uint32_t, int>> sorted(
        g_func_counts.begin(), g_func_counts.end());
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });

    int shown = 0;
    for (const auto& [addr, count] : sorted) {
        std::fprintf(stderr,
            "[PC-TRACE]   0x%08X  calls=%d\n", addr, count);
        if (++shown >= 20) break;
    }
    std::fprintf(stderr, "[PC-TRACE] === END DUMP ===\n");
}
