#include "hle/psp_hle.h"
#include "hle/psp_hle_kernel.h"
#include "psp_memory.h"
#include "psp_scheduler.h"
#include "recomp.h"

#include <cstdio>
#include <cstring>
#include <strings.h>
#include <ctime>
#include <chrono>
#include <thread>
#include <vector>

// ================================================================
// Catch-all HLE stubs for remaining Patapon NIDs.
// Groups: cache, RTC, audio, ATRAC, module mgmt, UMD, network,
//         MPEG, SAS, HTTP, SSL, impose, PSMF, stdio, interrupt,
//         kernel_library, loadexec, sceUtility.
// ================================================================

// ---- Kernel Memory Bump Allocator ----
// Starts after BOOT_MODULE_ADDR + NATIVE_MODULE_SIZE, grows upward.
// Kernel memory range: 0x08000000 - 0x083FFFFF (4MB)
static uint32_t g_kernel_heap_pos =
    BOOT_MODULE_ADDR + NATIVE_MODULE_SIZE;
static constexpr uint32_t KERNEL_MEM_END = 0x08400000U;

uint32_t psp_alloc_kernel_memory(uint32_t size) {
    uint32_t aligned = (size + 0xFU) & ~0xFU;  // 16-byte align
    if (g_kernel_heap_pos + aligned > KERNEL_MEM_END) {
        return 0;  // OOM
    }
    uint32_t addr = g_kernel_heap_pos;
    g_kernel_heap_pos += aligned;
    return addr;
}

uint32_t psp_get_boot_module_gp() {
    return 0x08A50D20U;  // Patapon module GP
}

// ---- Cache Operations (all no-ops) ----

static void hle_sceKernelDcacheWritebackAll(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelDcacheWritebackInvalidateAll(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelDcacheWritebackRange(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- RTC ----

static void hle_sceRtcGetAccumulativeTime(
    uint8_t* rdram, recomp_context* ctx
) {
    // Return current microsecond timestamp as 64-bit in v0:v1
    auto now = std::chrono::steady_clock::now();
    uint64_t us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count());
    ctx->r[2] = static_cast<int32_t>(us & 0xFFFFFFFF);
    ctx->r[3] = static_cast<int32_t>((us >> 32) & 0xFFFFFFFF);
    (void)rdram;
}

static void hle_sceRtcGetCurrentClockLocalTime(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t time_ptr = static_cast<uint32_t>(ctx->r[4]);

    if (time_ptr != 0) {
        // ScePspDateTime: year(u16), month(u16), day(u16),
        //   hour(u16), minute(u16), second(u16), microsecond(u32)
        time_t now = std::time(nullptr);
        struct tm* local = std::localtime(&now);
        if (local) {
            psp_mem_write<uint16_t>(rdram, time_ptr,
                static_cast<uint16_t>(local->tm_year + 1900));
            psp_mem_write<uint16_t>(rdram, time_ptr + 2,
                static_cast<uint16_t>(local->tm_mon + 1));
            psp_mem_write<uint16_t>(rdram, time_ptr + 4,
                static_cast<uint16_t>(local->tm_mday));
            psp_mem_write<uint16_t>(rdram, time_ptr + 6,
                static_cast<uint16_t>(local->tm_hour));
            psp_mem_write<uint16_t>(rdram, time_ptr + 8,
                static_cast<uint16_t>(local->tm_min));
            psp_mem_write<uint16_t>(rdram, time_ptr + 10,
                static_cast<uint16_t>(local->tm_sec));
            psp_mem_write<uint32_t>(rdram, time_ptr + 12, 0);
        }
    }

    ctx->r[2] = SCE_OK;
}

static void hle_sceKernelLibcTime(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t out_ptr = static_cast<uint32_t>(ctx->r[4]);
    int32_t t = static_cast<int32_t>(std::time(nullptr));

    if (out_ptr != 0) {
        psp_mem_write<int32_t>(rdram, out_ptr, t);
    }

    ctx->r[2] = t;
}

static void hle_sceKernelLibcClock(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = static_cast<int32_t>(std::clock());
    (void)rdram;
}

static void hle_sceKernelLibcGettimeofday(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t tv_ptr = static_cast<uint32_t>(ctx->r[4]);

    if (tv_ptr != 0) {
        auto now = std::chrono::system_clock::now();
        auto secs = std::chrono::duration_cast<
            std::chrono::seconds>(
                now.time_since_epoch()).count();
        auto usecs = std::chrono::duration_cast<
            std::chrono::microseconds>(
                now.time_since_epoch()).count() % 1000000;
        // struct timeval: tv_sec(u32), tv_usec(u32)
        psp_mem_write<uint32_t>(rdram, tv_ptr,
            static_cast<uint32_t>(secs));
        psp_mem_write<uint32_t>(rdram, tv_ptr + 4,
            static_cast<uint32_t>(usecs));
    }

    ctx->r[2] = SCE_OK;
}

// ---- Audio (timing only; no rendering yet) ----
// Issue #29: the *Blocking output calls must pace the caller at the
// hardware playback rate (44.1kHz). Returning instantly made the PCM
// loop spin at kHz, starving everything else.

static constexpr int AUDIO_OUTPUT_SAMPLE_RATE = 44100;
static constexpr int AUDIO_CHANNEL_COUNT = 8;
// Valid PSP sample counts: 17..4111 (sceAudioOutput2Reserve contract).
static constexpr int AUDIO_MIN_SAMPLES = 17;
static constexpr int AUDIO_MAX_SAMPLES = 4111;

// Sample count from sceAudioOutput2Reserve (default one 1024 grain
// if Reserve was never seen).
static int g_output2_samples = 1024;
// Per-channel sample counts from sceAudioChReserve.
static int g_channel_samples[AUDIO_CHANNEL_COUNT] = {
    1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024,
};

/// Block the calling thread for the playback duration of `samples`
/// samples at 44.1kHz. Mirrors the hle_sceKernelDelayThread blocking
/// idiom (psp_hle_kernel_thread.cpp): dispatch pending IO callbacks,
/// yield, then sleep.
static void audio_block_for_samples(
    uint8_t* rdram, recomp_context* ctx, int64_t samples
) {
    psp_kernel_check_callbacks(rdram, ctx);
    sched_yield_point();
    std::this_thread::sleep_for(std::chrono::microseconds(
        samples * 1000000LL / AUDIO_OUTPUT_SAMPLE_RATE));
}

static void hle_sceAudioOutputBlocking(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t channel = static_cast<uint32_t>(ctx->r[4]);
    // Preserve existing return convention (a1) before callbacks can
    // clobber argument registers.
    int32_t ret = ctx->r[5];
    int samples = (channel < AUDIO_CHANNEL_COUNT)
        ? g_channel_samples[channel] : 1024;
    audio_block_for_samples(rdram, ctx, samples);
    ctx->r[2] = ret;
}

static void hle_sceAudioOutputPannedBlocking(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t channel = static_cast<uint32_t>(ctx->r[4]);
    // Preserve existing return convention (a3) before callbacks can
    // clobber argument registers.
    int32_t ret = ctx->r[7];
    int samples = (channel < AUDIO_CHANNEL_COUNT)
        ? g_channel_samples[channel] : 1024;
    audio_block_for_samples(rdram, ctx, samples);
    ctx->r[2] = ret;
}

static void hle_sceAudioChReserve(
    uint8_t* rdram, recomp_context* ctx
) {
    int32_t channel = ctx->r[4];
    int32_t samples = ctx->r[5];
    if (channel < 0 || channel >= AUDIO_CHANNEL_COUNT) {
        channel = 0;  // auto-allocate / out-of-range: use channel 0
    }
    if (samples >= AUDIO_MIN_SAMPLES && samples <= AUDIO_MAX_SAMPLES) {
        g_channel_samples[channel] = samples;
    }
    ctx->r[2] = channel;
    (void)rdram;
}

static void hle_sceAudioChRelease(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceAudioOutput2Reserve(
    uint8_t* rdram, recomp_context* ctx
) {
    int32_t samples = ctx->r[4] & 0x7FFFFFFF;
    if (samples >= AUDIO_MIN_SAMPLES && samples <= AUDIO_MAX_SAMPLES) {
        g_output2_samples = samples;
    }
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceAudioOutput2OutputBlocking(
    uint8_t* rdram, recomp_context* ctx
) {
    // a0=vol, a1=buf. buf==0 is the pre-Release drain: sleep one grain
    // and report success, same as a normal grain.
    audio_block_for_samples(rdram, ctx, g_output2_samples);
    ctx->r[2] = SCE_OK;
}

static void hle_sceAudioOutput2Release(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceAudioOutputPanned(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceAudioChangeChannelConfig(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceAudioGetChannelRestLength(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 0;  // No samples remaining
    (void)rdram;
}

static void hle_sceAudioChangeChannelVolume(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceAudioSetChannelDataLen(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- ATRAC3plus (audio codec stubs) ----

static int g_atrac_next_id = 1;

static void hle_sceAtracGetAtracID(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = g_atrac_next_id++;
    (void)rdram;
}

static void hle_sceAtracReleaseAtracID(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceAtracSetData(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceAtracReinit(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ATRAC3 frames decode 1024 samples each.
static constexpr int32_t ATRAC_SAMPLES_PER_FRAME = 1024;
// Stereo s16 PCM for one frame: 1024 samples * 2 ch * 2 bytes.
static constexpr uint32_t ATRAC_PCM_BYTES = 4096;
// Grains of silence before we report the stream finished.
static constexpr uint32_t ATRAC_FINISH_GRAINS = 512;

/// Zero-fill one frame of PCM at the guest decode buffer (clamped
/// to guest memory, mirroring sas_zero_output).
static void atrac_zero_pcm(uint8_t* rdram, uint32_t out_addr) {
    // NULL-page rejection, consistent with the recomp.h accessor guard.
    if (out_addr < 0x00010000U) return;
    uint32_t bytes = ATRAC_PCM_BYTES;
    uint32_t off = out_addr & PSP_ADDR_MASK;
    if (off + bytes > PSP_MEM_SIZE) {
        bytes = static_cast<uint32_t>(PSP_MEM_SIZE) - off;
    }
    std::memset(rdram + off, 0, bytes);
}

static void hle_sceAtracDecodeData(
    uint8_t* rdram, recomp_context* ctx
) {
    // sceAtracDecodeData(atracID, u16* outPcm, u32* outSamples,
    //                    u32* outEnd, u32* outRemainFrame)
    // a0=r[4] id, a1=r[5] pcm, a2=r[6] samples,
    // a3=r[7] end, t0=r[8] remainFrame (PSP HLE arg 5)
    uint32_t pcm_ptr = static_cast<uint32_t>(ctx->r[5]);
    uint32_t samples_written_ptr =
        static_cast<uint32_t>(ctx->r[6]);
    uint32_t end_ptr =
        static_cast<uint32_t>(ctx->r[7]);
    uint32_t remain_ptr = static_cast<uint32_t>(ctx->r[8]);

    atrac_zero_pcm(rdram, pcm_ptr);
    if (samples_written_ptr != 0) {
        psp_mem_write<int32_t>(
            rdram, samples_written_ptr, ATRAC_SAMPLES_PER_FRAME);
    }
    if (end_ptr != 0) {
        // Bounded silence: report "not finished" for a while so the
        // decode worker keeps a sane cadence, then signal end.
        static uint32_t s_grains = 0;
        int32_t finished = (++s_grains >= ATRAC_FINISH_GRAINS) ? 1 : 0;
        psp_mem_write<int32_t>(rdram, end_ptr, finished);
    }
    if (remain_ptr != 0) {
        psp_mem_write<int32_t>(rdram, remain_ptr, -1);
    }

    ctx->r[2] = SCE_OK;
}

static void hle_sceAtracGetNextSample(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t out_ptr = static_cast<uint32_t>(ctx->r[5]);
    if (out_ptr != 0) {
        // ATRAC3 decodes 1024 samples per frame (PPSSPP semantics).
        psp_mem_write<int32_t>(rdram, out_ptr, ATRAC_SAMPLES_PER_FRAME);
    }
    ctx->r[2] = SCE_OK;
}

static void hle_sceAtracGetStreamDataInfo(
    uint8_t* rdram, recomp_context* ctx
) {
    // sceAtracGetStreamDataInfo(atracID, u8** writePtr,
    //                           u32* writableBytes, u32* readOffset)
    // Must write all three out-params: the guest decode worker
    // otherwise consumes stale stack as writePtr/writableBytes/
    // readOffset and issues wild reads.
    uint32_t write_ptr_ptr = static_cast<uint32_t>(ctx->r[5]);
    uint32_t writable_ptr = static_cast<uint32_t>(ctx->r[6]);
    uint32_t read_off_ptr = static_cast<uint32_t>(ctx->r[7]);
    if (write_ptr_ptr != 0) {
        psp_mem_write<uint32_t>(rdram, write_ptr_ptr, 0);
    }
    if (writable_ptr != 0) {
        psp_mem_write<uint32_t>(rdram, writable_ptr, 0);
    }
    if (read_off_ptr != 0) {
        psp_mem_write<uint32_t>(rdram, read_off_ptr, 0);
    }
    ctx->r[2] = SCE_OK;
}

static void hle_sceAtracAddStreamData(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceAtracSetLoopNum(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceAtracGetRemainFrame(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t out_ptr = static_cast<uint32_t>(ctx->r[5]);
    if (out_ptr != 0) {
        // -1 = "all data in memory" (PPSSPP semantics) — tells the
        // guest it never needs to stream more data in.
        psp_mem_write<int32_t>(rdram, out_ptr, -1);
    }
    ctx->r[2] = SCE_OK;
}

// ---- Module Management ----

struct ModuleInfo {
    int uid;
    uint32_t native_module_addr;  // Address in rdram of NativeModule
    uint32_t entry_addr;          // 0xFFFFFFFF for fake/PRX modules
    char name[28];
};

static std::vector<ModuleInfo> g_modules;
static int g_module_next_uid = 0x1000;

/// Initialize boot module in tracking list.
/// Called from psp_hle_register_utility().
static void init_boot_module_tracking() {
    ModuleInfo boot_mod{};
    boot_mod.uid = BOOT_MODULE_UID;
    boot_mod.native_module_addr = BOOT_MODULE_ADDR;
    boot_mod.entry_addr = 0x089ACCD0U;
    std::strncpy(boot_mod.name, "Labo", sizeof(boot_mod.name));
    g_modules.push_back(boot_mod);
}

/// Check if a path refers to BOOT.BIN.
/// Primary: case-insensitive last-component match.
/// Fallback: scan entire path for "BOOT.BIN" substring.
/// Handles "0sdisc0:0s/PSP_GAME/SYSDIR/BOOT.BIN" corruption
/// from LWL/LWR string-copy artifacts.
static bool path_is_boot_bin(const char* path) {
    // Primary: check last path component
    const char* slash = std::strrchr(path, '/');
    const char* filename = slash ? slash + 1 : path;
    const char* target = "BOOT.BIN";
    bool primary_match = true;
    for (int i = 0; target[i]; i++) {
        char c = filename[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c != target[i]) { primary_match = false; break; }
    }
    if (primary_match && filename[8] == '\0') return true;

    // Fallback: check if "BOOT.BIN" appears anywhere in path
    for (const char* p = path; *p; p++) {
        if ((*p == 'B' || *p == 'b') &&
            strncasecmp(p, "BOOT.BIN", 8) == 0) {
            return true;
        }
    }
    return false;
}

static void hle_sceKernelLoadModule(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t path_ptr = static_cast<uint32_t>(ctx->r[4]);
    const char* path = reinterpret_cast<const char*>(
        rdram + (path_ptr & PSP_ADDR_MASK));

    // Special case: BOOT.BIN self-load.
    // PPSSPP handles this via __KernelLoadExec (full restart).
    // Static recompiler: return existing boot module UID since
    // the game binary is already loaded and running.
    if (path_is_boot_bin(path)) {
        std::fprintf(stderr,
            "[HLE] sceKernelLoadModule(\"%s\") "
            "-> returning boot module uid=%d (self-load)\n",
            path, BOOT_MODULE_UID);
        ctx->r[2] = BOOT_MODULE_UID;
        return;
    }

    int uid = g_module_next_uid++;

    // Allocate a fake NativeModule in kernel memory
    uint32_t mod_addr =
        psp_alloc_kernel_memory(NATIVE_MODULE_SIZE);
    if (mod_addr != 0) {
        std::memset(
            rdram + (mod_addr & PSP_ADDR_MASK),
            0, NATIVE_MODULE_SIZE);
        // entry_addr = 0xFFFFFFFF (fake module, no entry)
        psp_mem_write<uint32_t>(
            rdram, mod_addr + 0x64, 0xFFFFFFFFU);
        // status = 4 (MODULE_STATUS_STARTING)
        psp_mem_write<uint32_t>(
            rdram, mod_addr + 0x24, 4);
        // modid
        psp_mem_write<uint32_t>(
            rdram, mod_addr + 0x2C,
            static_cast<uint32_t>(uid));
        // Extract filename from path (last '/' component)
        const char* filename = path;
        const char* slash = std::strrchr(path, '/');
        if (slash) {
            filename = slash + 1;
        }
        std::strncpy(
            reinterpret_cast<char*>(
                rdram + ((mod_addr + 0x08) & PSP_ADDR_MASK)),
            filename, 27);
    }

    // Track module
    ModuleInfo info{};
    info.uid = uid;
    info.native_module_addr = mod_addr;
    info.entry_addr = 0xFFFFFFFFU;
    const char* fname = path;
    const char* sl = std::strrchr(path, '/');
    if (sl) { fname = sl + 1; }
    std::strncpy(info.name, fname, sizeof(info.name) - 1);
    g_modules.push_back(info);

    std::fprintf(stderr,
        "[HLE] sceKernelLoadModule(\"%s\") -> uid=%d (fake)\n",
        path, uid);
    ctx->r[2] = uid;
}

static void hle_sceKernelStartModule(
    uint8_t* rdram, recomp_context* ctx
) {
    int uid = ctx->r[4];
    const char* action = "ok";

    // Look up module in tracking list
    for (auto& mod : g_modules) {
        if (mod.uid == uid) {
            // Boot module special case: it is already loaded and
            // running. The game calls LoadModule("BOOT.BIN") which
            // returns BOOT_MODULE_UID, then StartModule on it.
            // PPSSPP returns 0 for already-started modules.
            if (uid == BOOT_MODULE_UID) {
                std::fprintf(stderr,
                    "[HLE] sceKernelStartModule(uid=%d) "
                    "-> already started (boot module), "
                    "returning 0\n", uid);
                ctx->r[2] = SCE_OK;
                return;
            }

            if (mod.entry_addr == 0xFFFFFFFFU) {
                // Fake module: set status to STARTED, skip thread
                if (mod.native_module_addr != 0) {
                    psp_mem_write<uint32_t>(
                        rdram,
                        mod.native_module_addr + 0x24, 5);
                }
                action = "skipped (fake)";
            }
            break;
        }
    }

    std::fprintf(stderr,
        "[HLE] sceKernelStartModule(uid=%d) -> %s\n",
        uid, action);
    ctx->r[2] = SCE_OK;
}

static void hle_sceKernelStopModule(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelUnloadModule(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelStopUnloadSelfModuleWithStatus(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelGetModuleIdByAddress(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t addr = static_cast<uint32_t>(ctx->r[4]);
    // Patapon text segment: 0x08804000 to 0x08804000 + 0x244D30
    constexpr uint32_t TEXT_START = 0x08804000U;
    constexpr uint32_t TEXT_END = TEXT_START + 0x244D30U;
    if (addr >= TEXT_START && addr < TEXT_END) {
        ctx->r[2] = BOOT_MODULE_UID;
    } else {
        // Fallback: only one real module exists
        ctx->r[2] = BOOT_MODULE_UID;
    }
    (void)rdram;
}

static void hle_sceKernelGetModuleId(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = BOOT_MODULE_UID;
    (void)rdram;
}

static void hle_sceKernelQueryModuleInfo(
    uint8_t* rdram, recomp_context* ctx
) {
    int uid = ctx->r[4];
    uint32_t info_addr = static_cast<uint32_t>(ctx->r[5]);

    if (info_addr == 0) {
        std::fprintf(stderr,
            "[HLE] sceKernelQueryModuleInfo(uid=%d) "
            "-> EINVAL (null infoAddr)\n", uid);
        ctx->r[2] = SCE_ERROR_ERRNO_EINVAL;
        return;
    }

    // Find the module in tracking list
    uint32_t native_addr = 0;
    bool found = false;
    for (const auto& mod : g_modules) {
        if (mod.uid == uid) {
            native_addr = mod.native_module_addr;
            found = true;
            break;
        }
    }

    if (!found) {
        std::fprintf(stderr,
            "[HLE] sceKernelQueryModuleInfo(uid=%d) "
            "-> NOT_FOUND_MODULE\n", uid);
        ctx->r[2] = SCE_KERNEL_ERROR_NOT_FOUND_MODULE;
        return;
    }

    // Read fields from NativeModule in rdram and write
    // to SceKernelModuleInfo output struct at info_addr.
    //
    // NativeModule (source)        SceKernelModuleInfo (dest)
    // +0x7C nsegment            -> +0x0004 nsegment (u32)
    // +0x80 segaddr[0..3]       -> +0x0008 segmentaddr[0..3]
    // +0x90 segsize[0..3]       -> +0x0018 segmentsize[0..3]
    // +0x50 entry_addr          -> +0x0028 entry_addr (u32)
    // +0x68 gp_value            -> +0x002C gp_value (u32)
    // +0x6C text_addr           -> +0x0030 text_addr (u32)
    // +0x70 text_size           -> +0x0034 text_size (u32)
    //                              +0x0038 data_size = 0
    //                              +0x003C bss_size = 0
    // +0x04 attribute           -> +0x0040 attribute (u16)
    // +0x06 version             -> +0x0042 version (u8[2])
    // +0x08 name                -> +0x0044 name (28 bytes)

    // nsegment
    uint32_t nseg = psp_mem_read<uint32_t>(
        rdram, native_addr + 0x7C);
    psp_mem_write<uint32_t>(rdram, info_addr + 0x04, nseg);

    // segmentaddr[0..3]
    for (int i = 0; i < 4; i++) {
        uint32_t sa = psp_mem_read<uint32_t>(
            rdram, native_addr + 0x80 + i * 4);
        psp_mem_write<uint32_t>(
            rdram, info_addr + 0x08 + i * 4, sa);
    }

    // segmentsize[0..3]
    for (int i = 0; i < 4; i++) {
        uint32_t ss = psp_mem_read<uint32_t>(
            rdram, native_addr + 0x90 + i * 4);
        psp_mem_write<uint32_t>(
            rdram, info_addr + 0x18 + i * 4, ss);
    }

    // entry_addr (from module_start_func at +0x50)
    uint32_t entry = psp_mem_read<uint32_t>(
        rdram, native_addr + 0x50);
    psp_mem_write<uint32_t>(rdram, info_addr + 0x28, entry);

    // gp_value
    uint32_t gp = psp_mem_read<uint32_t>(
        rdram, native_addr + 0x68);
    psp_mem_write<uint32_t>(rdram, info_addr + 0x2C, gp);

    // text_addr
    uint32_t ta = psp_mem_read<uint32_t>(
        rdram, native_addr + 0x6C);
    psp_mem_write<uint32_t>(rdram, info_addr + 0x30, ta);

    // text_size
    uint32_t ts = psp_mem_read<uint32_t>(
        rdram, native_addr + 0x70);
    psp_mem_write<uint32_t>(rdram, info_addr + 0x34, ts);

    // data_size = 0, bss_size = 0 (not tracked in NativeModule)
    psp_mem_write<uint32_t>(rdram, info_addr + 0x38, 0);
    psp_mem_write<uint32_t>(rdram, info_addr + 0x3C, 0);

    // attribute
    uint16_t attr = psp_mem_read<uint16_t>(
        rdram, native_addr + 0x04);
    psp_mem_write<uint16_t>(rdram, info_addr + 0x40, attr);

    // version[2]
    uint8_t v0 = psp_mem_read<uint8_t>(
        rdram, native_addr + 0x06);
    uint8_t v1 = psp_mem_read<uint8_t>(
        rdram, native_addr + 0x07);
    psp_mem_write<uint8_t>(rdram, info_addr + 0x42, v0);
    psp_mem_write<uint8_t>(rdram, info_addr + 0x43, v1);

    // name (28 bytes)
    std::memcpy(
        rdram + ((info_addr + 0x44) & PSP_ADDR_MASK),
        rdram + ((native_addr + 0x08) & PSP_ADDR_MASK),
        28);

    std::fprintf(stderr,
        "[HLE] sceKernelQueryModuleInfo(uid=%d) -> OK\n",
        uid);
    ctx->r[2] = SCE_OK;
}

// ---- UMD (Universal Media Disc) ----

static void hle_sceUmdCheckMedium(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 1;  // Disc is present
    (void)rdram;
}

static void hle_sceUmdGetDriveStat(
    uint8_t* rdram, recomp_context* ctx
) {
    // PSP_UMD_PRESENT | PSP_UMD_READY | PSP_UMD_READABLE = 0x02 | 0x10 | 0x20 = 0x32
    // Bit values from PPSSPP sceUmd.h:
    //   0x01 = NOT_PRESENT, 0x02 = PRESENT, 0x04 = CHANGED,
    //   0x08 = NOT_READY,   0x10 = READY,   0x20 = READABLE
    ctx->r[2] = 0x02 | 0x10 | 0x20;
    (void)rdram;
}

static void hle_sceUmdActivate(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUmdWaitDriveStat(
    uint8_t* rdram, recomp_context* ctx
) {
    sched_yield_point();
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUmdWaitDriveStatCB(
    uint8_t* rdram, recomp_context* ctx
) {
    psp_kernel_check_callbacks(rdram, ctx);
    sched_yield_point();
    ctx->r[2] = SCE_OK;
}

static void hle_sceUmdGetErrorStat(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 0;  // No error
    (void)rdram;
}

static void hle_sceUmdRegisterUMDCallBack(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUmdUnRegisterUMDCallBack(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- Networking (all init/term no-ops) ----

static void hle_sceNetInit(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceNetTerm(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceNetInetInit(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceNetInetTerm(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceNetApctlInit(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceNetApctlTerm(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceNetResolverInit(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceNetResolverTerm(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- HTTP/SSL ----

static void hle_sceHttpInit(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceHttpEnd(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceHttpsInit(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceHttpsEnd(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceHttpsLoadDefaultCert(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceHttpSaveSystemCookie(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceHttpLoadSystemCookie(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceSslInit(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceSslEnd(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- MPEG (video playback stubs) ----

static int g_mpeg_next_uid = 1;

static void hle_sceMpegInit(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceMpegFinish(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceMpegCreate(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceMpegDelete(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceMpegQueryMemSize(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 0x10000;  // 64KB
    (void)rdram;
}

static void hle_sceMpegQueryStreamOffset(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceMpegQueryStreamSize(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceMpegRegistStream(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = g_mpeg_next_uid++;
    (void)rdram;
}

static void hle_sceMpegUnRegistStream(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceMpegFlushAllStream(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceMpegMallocAvcEsBuf(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 1;  // Return valid buffer ID
    (void)rdram;
}

static void hle_sceMpegFreeAvcEsBuf(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceMpegInitAu(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceMpegGetAtracAu(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceMpegGetAvcAu(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceMpegAtracDecode(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceMpegAvcDecodeYCbCr(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceMpegAvcDecodeStopYCbCr(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceMpegAvcQueryYCbCrSize(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceMpegAvcInitYCbCr(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceMpegAvcCsc(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceMpegQueryAtracEsSize(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceMpegRingbufferConstruct(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceMpegRingbufferDestruct(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceMpegRingbufferQueryMemSize(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 0x10000;  // 64KB
    (void)rdram;
}

static void hle_sceMpegRingbufferPut(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceMpegRingbufferAvailableSize(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 0x10000;  // Plenty available
    (void)rdram;
}

// ---- PSMF (PlayStation Media Framework) ----

static void hle_scePsmfSetPsmf(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_scePsmfGetVideoInfo(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_scePsmfSpecifyStream(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_scePsmfGetNumberOfSpecificStreams(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 0;  // No streams
    (void)rdram;
}

static void hle_scePsmfGetCurrentStreamType(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_scePsmfGetPsmfVersion(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 1;  // Version 1
    (void)rdram;
}

static void hle_scePsmfGetNumberOfStreams(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 0;
    (void)rdram;
}

// ---- SAS (Software Audio Synthesis) ----
// Moved to psp_hle_sas.cpp (issue #29: minimal voice state machine).

// ---- Stdio ----

static void hle_sceKernelStdin(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 0;  // fd 0
    (void)rdram;
}

static void hle_sceKernelStdout(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 1;  // fd 1
    (void)rdram;
}

static void hle_sceKernelStderr(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 2;  // fd 2
    (void)rdram;
}

// ---- Interrupt Manager ----

static void hle_sceKernelRegisterSubIntrHandler(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelReleaseSubIntrHandler(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelEnableSubIntr(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- Kernel_Library ----

static void hle_sceKernelCpuSuspendIntr(
    uint8_t* rdram, recomp_context* ctx
) {
    // Return previous interrupt state (0 = enabled)
    ctx->r[2] = 0;
    (void)rdram;
}

static void hle_sceKernelCpuResumeIntr(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- LoadExec ----

static void hle_sceKernelExitGame(
    uint8_t* rdram, recomp_context* ctx
) {
    std::fprintf(stderr, "[HLE] sceKernelExitGame called\n");
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelRegisterExitCallback(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- Impose ----

static void hle_sceImposeGetLanguageMode(
    uint8_t* rdram, recomp_context* ctx
) {
    // a0 = lang_ptr, a1 = button_ptr
    uint32_t lang_ptr = static_cast<uint32_t>(ctx->r[4]);
    uint32_t button_ptr = static_cast<uint32_t>(ctx->r[5]);

    if (lang_ptr != 0) {
        psp_mem_write<int32_t>(rdram, lang_ptr, 1);  // English
    }
    if (button_ptr != 0) {
        psp_mem_write<int32_t>(rdram, button_ptr, 1);  // Cross=confirm
    }

    ctx->r[2] = SCE_OK;
}

static void hle_sceImposeSetLanguageMode(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceImposeSetUMDPopup(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- Suspend ----

static void hle_sceKernelPowerTick(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- sceUtility ----

static void hle_sceUtilityLoadModule(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUtilityUnloadModule(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUtilityGetSystemParamInt(
    uint8_t* rdram, recomp_context* ctx
) {
    int32_t param_id = ctx->r[4];
    uint32_t value_ptr = static_cast<uint32_t>(ctx->r[5]);

    int32_t value = 0;
    switch (param_id) {
        case 1: value = 1; break;  // Language = English
        case 5: value = 0; break;  // Date format = YYYYMMDD
        case 6: value = 0; break;  // Time format = 24h
        case 7: value = 0; break;  // Timezone offset
        case 8: value = 1; break;  // Daylight saving
        case 9: value = 1; break;  // Nickname (not impl'd via int)
        default: break;
    }

    if (value_ptr != 0) {
        psp_mem_write<int32_t>(rdram, value_ptr, value);
    }

    ctx->r[2] = SCE_OK;
}

// Utility dialog stubs (savedata, msg dialog, OSK, HTML viewer)
// All return "not running" status (4 = FINISHED/NONE)
static constexpr int32_t PSP_UTILITY_STATUS_NONE = 0;

static void hle_sceUtilitySavedataInitStart(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUtilitySavedataGetStatus(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = PSP_UTILITY_STATUS_NONE;
    (void)rdram;
}

static void hle_sceUtilitySavedataShutdownStart(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUtilitySavedataUpdate(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUtilityMsgDialogInitStart(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUtilityMsgDialogGetStatus(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = PSP_UTILITY_STATUS_NONE;
    (void)rdram;
}

static void hle_sceUtilityMsgDialogShutdownStart(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUtilityMsgDialogUpdate(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUtilityOskInitStart(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUtilityOskGetStatus(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = PSP_UTILITY_STATUS_NONE;
    (void)rdram;
}

static void hle_sceUtilityOskShutdownStart(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUtilityOskUpdate(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUtilityHtmlViewerInitStart(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUtilityHtmlViewerGetStatus(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = PSP_UTILITY_STATUS_NONE;
    (void)rdram;
}

static void hle_sceUtilityHtmlViewerShutdownStart(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUtilityHtmlViewerUpdate(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- MsgPipe (Kernel message pipes) ----

static int g_msgpipe_next_uid = 0x2000;

static void hle_sceKernelCreateMsgPipe(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = g_msgpipe_next_uid++;
    (void)rdram;
}

static void hle_sceKernelDeleteMsgPipe(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelSendMsgPipe(
    uint8_t* rdram, recomp_context* ctx
) {
    sched_yield_point();
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelTryReceiveMsgPipe(
    uint8_t* rdram, recomp_context* ctx
) {
    // Non-blocking receive: return "no data"
    ctx->r[2] = SCE_KERNEL_ERROR_WAIT_TIMEOUT;
    (void)rdram;
}

// ---- Registration ----

void psp_hle_register_utility() {
    // Initialize boot module tracking
    init_boot_module_tracking();

    // Cache operations
    psp_hle_register("sceKernelDcacheWritebackAll",
                      hle_sceKernelDcacheWritebackAll);
    psp_hle_register("sceKernelDcacheWritebackInvalidateAll",
                      hle_sceKernelDcacheWritebackInvalidateAll);
    psp_hle_register("sceKernelDcacheWritebackRange",
                      hle_sceKernelDcacheWritebackRange);

    // RTC
    psp_hle_register("sceRtcGetAccumulativeTime",
                      hle_sceRtcGetAccumulativeTime);
    psp_hle_register("sceRtcGetCurrentClockLocalTime",
                      hle_sceRtcGetCurrentClockLocalTime);
    psp_hle_register("sceKernelLibcTime",
                      hle_sceKernelLibcTime);
    psp_hle_register("sceKernelLibcClock",
                      hle_sceKernelLibcClock);
    psp_hle_register("sceKernelLibcGettimeofday",
                      hle_sceKernelLibcGettimeofday);

    // Audio
    psp_hle_register("sceAudioOutputBlocking",
                      hle_sceAudioOutputBlocking);
    psp_hle_register("sceAudioOutputPannedBlocking",
                      hle_sceAudioOutputPannedBlocking);
    psp_hle_register("sceAudioChReserve",
                      hle_sceAudioChReserve);
    psp_hle_register("sceAudioChRelease",
                      hle_sceAudioChRelease);
    psp_hle_register("sceAudioOutput2Reserve",
                      hle_sceAudioOutput2Reserve);
    psp_hle_register("sceAudioOutput2OutputBlocking",
                      hle_sceAudioOutput2OutputBlocking);
    psp_hle_register("sceAudioOutput2Release",
                      hle_sceAudioOutput2Release);
    psp_hle_register("sceAudioOutputPanned",
                      hle_sceAudioOutputPanned);
    psp_hle_register("sceAudioChangeChannelConfig",
                      hle_sceAudioChangeChannelConfig);
    psp_hle_register("sceAudioGetChannelRestLength",
                      hle_sceAudioGetChannelRestLength);
    psp_hle_register("sceAudioChangeChannelVolume",
                      hle_sceAudioChangeChannelVolume);
    psp_hle_register("sceAudioSetChannelDataLen",
                      hle_sceAudioSetChannelDataLen);

    // ATRAC3plus
    psp_hle_register("sceAtracGetAtracID",
                      hle_sceAtracGetAtracID);
    psp_hle_register("sceAtracReleaseAtracID",
                      hle_sceAtracReleaseAtracID);
    psp_hle_register("sceAtracSetData",
                      hle_sceAtracSetData);
    psp_hle_register("sceAtracReinit",
                      hle_sceAtracReinit);
    psp_hle_register("sceAtracDecodeData",
                      hle_sceAtracDecodeData);
    psp_hle_register("sceAtracGetNextSample",
                      hle_sceAtracGetNextSample);
    psp_hle_register("sceAtracGetStreamDataInfo",
                      hle_sceAtracGetStreamDataInfo);
    psp_hle_register("sceAtracAddStreamData",
                      hle_sceAtracAddStreamData);
    psp_hle_register("sceAtracSetLoopNum",
                      hle_sceAtracSetLoopNum);
    psp_hle_register("sceAtracGetRemainFrame",
                      hle_sceAtracGetRemainFrame);

    // Module management
    psp_hle_register("sceKernelLoadModule",
                      hle_sceKernelLoadModule);
    psp_hle_register("sceKernelStartModule",
                      hle_sceKernelStartModule);
    psp_hle_register("sceKernelStopModule",
                      hle_sceKernelStopModule);
    psp_hle_register("sceKernelUnloadModule",
                      hle_sceKernelUnloadModule);
    psp_hle_register(
        "sceKernelStopUnloadSelfModuleWithStatus",
        hle_sceKernelStopUnloadSelfModuleWithStatus);
    psp_hle_register("sceKernelGetModuleIdByAddress",
                      hle_sceKernelGetModuleIdByAddress);
    psp_hle_register("sceKernelGetModuleId",
                      hle_sceKernelGetModuleId);
    psp_hle_register("sceKernelQueryModuleInfo",
                      hle_sceKernelQueryModuleInfo);

    // UMD
    psp_hle_register("sceUmdCheckMedium",
                      hle_sceUmdCheckMedium);
    psp_hle_register("sceUmdGetDriveStat",
                      hle_sceUmdGetDriveStat);
    psp_hle_register("sceUmdActivate",
                      hle_sceUmdActivate);
    psp_hle_register("sceUmdWaitDriveStat",
                      hle_sceUmdWaitDriveStat);
    psp_hle_register("sceUmdWaitDriveStatCB",
                      hle_sceUmdWaitDriveStatCB);
    psp_hle_register("sceUmdGetErrorStat",
                      hle_sceUmdGetErrorStat);
    psp_hle_register("sceUmdRegisterUMDCallBack",
                      hle_sceUmdRegisterUMDCallBack);
    psp_hle_register("sceUmdUnRegisterUMDCallBack",
                      hle_sceUmdUnRegisterUMDCallBack);

    // Networking
    psp_hle_register("sceNetInit", hle_sceNetInit);
    psp_hle_register("sceNetTerm", hle_sceNetTerm);
    psp_hle_register("sceNetInetInit", hle_sceNetInetInit);
    psp_hle_register("sceNetInetTerm", hle_sceNetInetTerm);
    psp_hle_register("sceNetApctlInit", hle_sceNetApctlInit);
    psp_hle_register("sceNetApctlTerm", hle_sceNetApctlTerm);
    psp_hle_register("sceNetResolverInit",
                      hle_sceNetResolverInit);
    psp_hle_register("sceNetResolverTerm",
                      hle_sceNetResolverTerm);

    // HTTP/SSL
    psp_hle_register("sceHttpInit", hle_sceHttpInit);
    psp_hle_register("sceHttpEnd", hle_sceHttpEnd);
    psp_hle_register("sceHttpsInit", hle_sceHttpsInit);
    psp_hle_register("sceHttpsEnd", hle_sceHttpsEnd);
    psp_hle_register("sceHttpsLoadDefaultCert",
                      hle_sceHttpsLoadDefaultCert);
    psp_hle_register("sceHttpSaveSystemCookie",
                      hle_sceHttpSaveSystemCookie);
    psp_hle_register("sceHttpLoadSystemCookie",
                      hle_sceHttpLoadSystemCookie);
    psp_hle_register("sceSslInit", hle_sceSslInit);
    psp_hle_register("sceSslEnd", hle_sceSslEnd);

    // MPEG
    psp_hle_register("sceMpegInit", hle_sceMpegInit);
    psp_hle_register("sceMpegFinish", hle_sceMpegFinish);
    psp_hle_register("sceMpegCreate", hle_sceMpegCreate);
    psp_hle_register("sceMpegDelete", hle_sceMpegDelete);
    psp_hle_register("sceMpegQueryMemSize",
                      hle_sceMpegQueryMemSize);
    psp_hle_register("sceMpegQueryStreamOffset",
                      hle_sceMpegQueryStreamOffset);
    psp_hle_register("sceMpegQueryStreamSize",
                      hle_sceMpegQueryStreamSize);
    psp_hle_register("sceMpegRegistStream",
                      hle_sceMpegRegistStream);
    psp_hle_register("sceMpegUnRegistStream",
                      hle_sceMpegUnRegistStream);
    psp_hle_register("sceMpegFlushAllStream",
                      hle_sceMpegFlushAllStream);
    psp_hle_register("sceMpegMallocAvcEsBuf",
                      hle_sceMpegMallocAvcEsBuf);
    psp_hle_register("sceMpegFreeAvcEsBuf",
                      hle_sceMpegFreeAvcEsBuf);
    psp_hle_register("sceMpegInitAu",
                      hle_sceMpegInitAu);
    psp_hle_register("sceMpegGetAtracAu",
                      hle_sceMpegGetAtracAu);
    psp_hle_register("sceMpegGetAvcAu",
                      hle_sceMpegGetAvcAu);
    psp_hle_register("sceMpegAtracDecode",
                      hle_sceMpegAtracDecode);
    psp_hle_register("sceMpegAvcDecodeYCbCr",
                      hle_sceMpegAvcDecodeYCbCr);
    psp_hle_register("sceMpegAvcDecodeStopYCbCr",
                      hle_sceMpegAvcDecodeStopYCbCr);
    psp_hle_register("sceMpegAvcQueryYCbCrSize",
                      hle_sceMpegAvcQueryYCbCrSize);
    psp_hle_register("sceMpegAvcInitYCbCr",
                      hle_sceMpegAvcInitYCbCr);
    psp_hle_register("sceMpegAvcCsc",
                      hle_sceMpegAvcCsc);
    psp_hle_register("sceMpegQueryAtracEsSize",
                      hle_sceMpegQueryAtracEsSize);
    psp_hle_register("sceMpegRingbufferConstruct",
                      hle_sceMpegRingbufferConstruct);
    psp_hle_register("sceMpegRingbufferDestruct",
                      hle_sceMpegRingbufferDestruct);
    psp_hle_register("sceMpegRingbufferQueryMemSize",
                      hle_sceMpegRingbufferQueryMemSize);
    psp_hle_register("sceMpegRingbufferPut",
                      hle_sceMpegRingbufferPut);
    psp_hle_register("sceMpegRingbufferAvailableSize",
                      hle_sceMpegRingbufferAvailableSize);

    // PSMF
    psp_hle_register("scePsmfSetPsmf",
                      hle_scePsmfSetPsmf);
    psp_hle_register("scePsmfGetVideoInfo",
                      hle_scePsmfGetVideoInfo);
    psp_hle_register("scePsmfSpecifyStream",
                      hle_scePsmfSpecifyStream);
    psp_hle_register("scePsmfGetNumberOfSpecificStreams",
                      hle_scePsmfGetNumberOfSpecificStreams);
    psp_hle_register("scePsmfGetCurrentStreamType",
                      hle_scePsmfGetCurrentStreamType);
    psp_hle_register("scePsmfGetPsmfVersion",
                      hle_scePsmfGetPsmfVersion);
    psp_hle_register("scePsmfGetNumberOfStreams",
                      hle_scePsmfGetNumberOfStreams);

    // SAS (Software Audio Synthesis) -- see psp_hle_sas.cpp
    // (registered by psp_hle_register_sas()).

    // Stdio
    psp_hle_register("sceKernelStdin", hle_sceKernelStdin);
    psp_hle_register("sceKernelStdout", hle_sceKernelStdout);
    psp_hle_register("sceKernelStderr", hle_sceKernelStderr);

    // Interrupt manager
    psp_hle_register("sceKernelRegisterSubIntrHandler",
                      hle_sceKernelRegisterSubIntrHandler);
    psp_hle_register("sceKernelReleaseSubIntrHandler",
                      hle_sceKernelReleaseSubIntrHandler);
    psp_hle_register("sceKernelEnableSubIntr",
                      hle_sceKernelEnableSubIntr);

    // Kernel_Library
    psp_hle_register("sceKernelCpuSuspendIntr",
                      hle_sceKernelCpuSuspendIntr);
    psp_hle_register("sceKernelCpuResumeIntr",
                      hle_sceKernelCpuResumeIntr);

    // LoadExec
    psp_hle_register("sceKernelExitGame",
                      hle_sceKernelExitGame);
    psp_hle_register("sceKernelRegisterExitCallback",
                      hle_sceKernelRegisterExitCallback);

    // Impose
    psp_hle_register("sceImposeGetLanguageMode",
                      hle_sceImposeGetLanguageMode);
    psp_hle_register("sceImposeSetLanguageMode",
                      hle_sceImposeSetLanguageMode);
    psp_hle_register("sceImposeSetUMDPopup",
                      hle_sceImposeSetUMDPopup);

    // Suspend
    psp_hle_register("sceKernelPowerTick",
                      hle_sceKernelPowerTick);

    // sceUtility
    psp_hle_register("sceUtilityLoadModule",
                      hle_sceUtilityLoadModule);
    psp_hle_register("sceUtilityUnloadModule",
                      hle_sceUtilityUnloadModule);
    psp_hle_register("sceUtilityGetSystemParamInt",
                      hle_sceUtilityGetSystemParamInt);
    psp_hle_register("sceUtilitySavedataInitStart",
                      hle_sceUtilitySavedataInitStart);
    psp_hle_register("sceUtilitySavedataGetStatus",
                      hle_sceUtilitySavedataGetStatus);
    psp_hle_register("sceUtilitySavedataShutdownStart",
                      hle_sceUtilitySavedataShutdownStart);
    psp_hle_register("sceUtilitySavedataUpdate",
                      hle_sceUtilitySavedataUpdate);
    psp_hle_register("sceUtilityMsgDialogInitStart",
                      hle_sceUtilityMsgDialogInitStart);
    psp_hle_register("sceUtilityMsgDialogGetStatus",
                      hle_sceUtilityMsgDialogGetStatus);
    psp_hle_register("sceUtilityMsgDialogShutdownStart",
                      hle_sceUtilityMsgDialogShutdownStart);
    psp_hle_register("sceUtilityMsgDialogUpdate",
                      hle_sceUtilityMsgDialogUpdate);
    psp_hle_register("sceUtilityOskInitStart",
                      hle_sceUtilityOskInitStart);
    psp_hle_register("sceUtilityOskGetStatus",
                      hle_sceUtilityOskGetStatus);
    psp_hle_register("sceUtilityOskShutdownStart",
                      hle_sceUtilityOskShutdownStart);
    psp_hle_register("sceUtilityOskUpdate",
                      hle_sceUtilityOskUpdate);
    psp_hle_register("sceUtilityHtmlViewerInitStart",
                      hle_sceUtilityHtmlViewerInitStart);
    psp_hle_register("sceUtilityHtmlViewerGetStatus",
                      hle_sceUtilityHtmlViewerGetStatus);
    psp_hle_register("sceUtilityHtmlViewerShutdownStart",
                      hle_sceUtilityHtmlViewerShutdownStart);
    psp_hle_register("sceUtilityHtmlViewerUpdate",
                      hle_sceUtilityHtmlViewerUpdate);

    // MsgPipe
    psp_hle_register("sceKernelCreateMsgPipe",
                      hle_sceKernelCreateMsgPipe);
    psp_hle_register("sceKernelDeleteMsgPipe",
                      hle_sceKernelDeleteMsgPipe);
    psp_hle_register("sceKernelSendMsgPipe",
                      hle_sceKernelSendMsgPipe);
    psp_hle_register("sceKernelTryReceiveMsgPipe",
                      hle_sceKernelTryReceiveMsgPipe);
}
