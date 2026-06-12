//! `recompile` and `dump` subcommand implementations.
//!
//! `run_recompile`: full pipeline from analysis.json to C++ project on disk.
//! `run_dump`: print selected analysis.json fields to stdout.

use std::collections::{HashMap, HashSet};
use std::path::Path;
use std::sync::Mutex;

use anyhow::Context;
use base64::Engine as _;
use base64::engine::general_purpose::STANDARD as B64;
use indicatif::{ProgressBar, ProgressStyle};

use psp_emitter::{
    CppGenerator,
    call_resolver::{build_import_map, build_func_map},
    mid_entry::{build_parent_name_map, emit_mid_entry_wrappers},
    emit_dispatch_table,
    emit_data_sections,
    emit_psp_call_constructors,
    emit_cmake_lists,
    emit_function_batches,
};
use psp_ir::{BasicBlock, DecodedFunction, MipsOp};
use psp_optimizer::{optimize, OptimizerConfig};
use psp_parser::analysis_json::{AnalysisJson, JsonFunction, JsonMidEntry};

use crate::config::{GameConfig, load_config};
use crate::report::{self, DecodeErrorEntry, DiscoveryCounts};

// -------------------------------------------------------------------------
// Public entry points
// -------------------------------------------------------------------------

/// Print selected analysis.json fields to stdout.
///
/// `what`: "functions" | "imports" | "segments" | "mid_entries" | "relocations"
pub fn run_dump(analysis_path: &Path, what: &str) -> anyhow::Result<()> {
    let text = std::fs::read_to_string(analysis_path)
        .with_context(|| format!("Cannot read {}", analysis_path.display()))?;
    let analysis: AnalysisJson = serde_json::from_str(&text)
        .with_context(|| format!("Invalid JSON in {}", analysis_path.display()))?;

    match what {
        "functions" => {
            for f in &analysis.functions {
                println!("{} {}", f.address, f.name);
            }
        }
        "imports" => {
            for imp in &analysis.imports {
                println!("{} {}", imp.nid, imp.name);
            }
        }
        "segments" => {
            for seg in &analysis.segments {
                println!("{} memsz={}", seg.p_vaddr, seg.p_memsz);
            }
        }
        "mid_entries" => {
            for me in &analysis.mid_entries {
                println!("{} parent={}", me.addr, me.parent_addr);
            }
        }
        "relocations" => {
            for rel in &analysis.relocations {
                println!("{} type={}", rel.offset, rel.r_type);
            }
        }
        other => anyhow::bail!(
            "Unknown --what value '{other}'. Valid: functions, imports, segments, \
             mid_entries, relocations"
        ),
    }
    Ok(())
}

/// Options for [`run_recompile`] beyond the input/output paths.
#[derive(Debug, Default)]
pub struct RecompileOptions {
    /// Optional TOML game config (stubs, skips, patches).
    pub config_path: Option<std::path::PathBuf>,
    /// Functions per generated .cpp file.
    pub batch_size: usize,
    /// If set, fail when the final function count differs (`--expect-functions`).
    pub expect_functions: Option<usize>,
    /// If set, fail when the final mid-entry count differs (`--expect-mid-entries`).
    pub expect_mid_entries: Option<usize>,
}

/// Everything the per-function emit closure consumes, computed once from
/// analysis.json. Shared by [`run_recompile`] and the single-function dump
/// (`crate::dump`) so both paths run the exact same pipeline (discovery,
/// force mid-entries, PSPRECOMP_CROSS_MID passes, name dedup) and therefore
/// produce byte-identical C++ for any given function.
pub(crate) struct PreparedEmission {
    pub(crate) analysis: AnalysisJson,
    pub(crate) config: GameConfig,
    pub(crate) segment_bytes: Vec<(u32, Vec<u8>)>,
    pub(crate) discovery: DiscoveryCounts,
    pub(crate) coalesced_owners: HashSet<u32>,
    pub(crate) import_map: HashMap<u32, String>,
    pub(crate) func_map: HashMap<u32, String>,
    pub(crate) unique_names: HashMap<u32, String>,
    pub(crate) mid_entry_addr_map: HashMap<u32, Vec<u32>>,
    pub(crate) data_xrefs: Vec<(u32, u32)>,
}

/// Load analysis.json and run every in-memory transformation that precedes
/// batch emission. Read-only with respect to the filesystem.
pub(crate) fn prepare_emission(
    analysis_path: &Path,
    config_path: Option<&Path>,
) -> anyhow::Result<PreparedEmission> {
    let mut analysis = load_analysis(analysis_path)?;
    let config = load_config(config_path)?;
    tracing::info!("Loaded {} functions from {}", analysis.functions.len(), analysis_path.display());

    // Decode segment bytes from base64 (before enhancement -- prologue scan needs raw bytes)
    let segment_bytes = decode_segment_bytes(&analysis);
    tracing::info!("Decoded {} segments", segment_bytes.len());

    // Enhanced function discovery: three-pass scan replaces vtable_miss_addresses.txt sidecar
    let discovery = enhance_function_discovery(&mut analysis, &segment_bytes);
    tracing::info!("Total functions after enhancement: {}", analysis.functions.len());

    // Force-inject mid-entries that Ghidra missed but are confirmed call targets
    // observed as repeated LOOKUP_MISS in the runtime. Mirrors FORCE_ENTRIES but
    // for mid-function entry points inside an existing parent function.
    inject_force_mid_entries(&mut analysis);

    // PSPRECOMP_CROSS_MID=1: coalesce Ghidra-over-split shared-frame siblings,
    // then run systematic cross-function mid-jump recovery (D2+D3a, 19G/19H).
    // GATED OFF by default (D1 baseline): landing it makes the BROKEN
    // decompressor at 0x089D7xxx RUN and stall at the D3 inflate-heap wall,
    // regressing the boot below the D1 baseline — see the doc comments on
    // `coalesce_split_frame_siblings` / `inject_cross_function_mid_jumps`.
    // Coalesce owners are threaded to the emit closure so the emitter applies
    // the Option-A LINK/RA model to them. PSPRECOMP_NO_COALESCE=1 disables
    // the merge only (A/B diagnostic).
    let mut coalesced_owners: HashSet<u32> = HashSet::new();
    if std::env::var("PSPRECOMP_CROSS_MID").as_deref() == Ok("1") {
        // DATA xrefs are needed for the decoder's jump-table promotion so the
        // re-decode matches the emitter's exact op stream.
        let data_xrefs_pre: Vec<(u32, u32)> = analysis.xrefs.iter()
            .filter(|x| x.ref_type == "DATA")
            .filter_map(|x| Some((parse_hex_u32(&x.from_addr)?, parse_hex_u32(&x.to_addr)?)))
            .collect();
        if std::env::var("PSPRECOMP_NO_COALESCE").as_deref() != Ok("1") {
            coalesced_owners =
                coalesce_split_frame_siblings(&mut analysis, &segment_bytes, &data_xrefs_pre);
        }
        inject_cross_function_mid_jumps(&mut analysis, &segment_bytes, &data_xrefs_pre);
    }

    // Lookup maps consumed by decode_and_emit_function_with_name.
    // Name dedup: Ghidra may produce multiple functions with the same name at
    // different addresses (e.g. thunk_FUN_xxx); collisions get _ADDR suffixes.
    let import_map = build_import_map(&analysis.imports);
    let unique_names = dedup_function_names(&analysis.functions);
    let func_map = build_func_map(&analysis.functions);

    // Mid-entry address map: parent_addr -> Vec<mid_entry_addr>
    let mut mid_entry_addr_map: HashMap<u32, Vec<u32>> = HashMap::new();
    for me in &analysis.mid_entries {
        let entry_addr = parse_hex_u32(&me.addr).unwrap_or(0);
        let parent_addr = parse_hex_u32(&me.parent_addr).unwrap_or(0);
        if entry_addr != 0 && parent_addr != 0 {
            mid_entry_addr_map.entry(parent_addr).or_default().push(entry_addr);
        }
    }
    tracing::info!("Built mid-entry addr map: {} parent functions", mid_entry_addr_map.len());

    // DATA xref table: (from_addr, to_addr) pairs for jump table detection
    let data_xrefs: Vec<(u32, u32)> = analysis.xrefs.iter()
        .filter(|x| x.ref_type == "DATA")
        .filter_map(|x| {
            let from = parse_hex_u32(&x.from_addr)?;
            let to = parse_hex_u32(&x.to_addr)?;
            Some((from, to))
        })
        .collect();
    tracing::info!("Loaded {} DATA xrefs for jump table detection", data_xrefs.len());

    Ok(PreparedEmission {
        analysis,
        config,
        segment_bytes,
        discovery,
        coalesced_owners,
        import_map,
        func_map,
        unique_names,
        mid_entry_addr_map,
        data_xrefs,
    })
}

/// Decode + emit exactly one function the way the batch closure does.
///
/// This is THE per-function emission path: `run_recompile`'s batch closure
/// and the single-function dump both call it, so their output is identical.
pub(crate) fn emit_one_function(
    prep: &PreparedEmission,
    func: &JsonFunction,
) -> (String, EmitDiagnostics) {
    let func_addr = parse_hex_u32(&func.address).unwrap_or(0);
    let unique_name = prep
        .unique_names
        .get(&func_addr)
        .cloned()
        .unwrap_or_else(|| func.name.clone());
    // PSPRECOMP_NO_RA_MODEL=1 keeps the structural merge but disables the
    // Option-A LINK/RA lowering (diagnostic isolating the two halves).
    let ra_off = std::env::var("PSPRECOMP_NO_RA_MODEL").as_deref() == Ok("1");
    let is_coalesced = !ra_off && prep.coalesced_owners.contains(&func_addr);
    decode_and_emit_function_with_name(
        func,
        &unique_name,
        &prep.segment_bytes,
        &prep.import_map,
        &prep.func_map,
        &prep.config,
        &prep.data_xrefs,
        &prep.mid_entry_addr_map,
        is_coalesced,
    )
}

/// Full recompile pipeline: analysis.json -> C++ project on disk.
pub fn run_recompile(
    analysis_path: &Path,
    output_dir: &Path,
    opts: &RecompileOptions,
) -> anyhow::Result<()> {
    let prep = prepare_emission(analysis_path, opts.config_path.as_deref())?;
    let effective_batch_size = prep.config.functions_per_file.unwrap_or(opts.batch_size);

    // Prepare output directories
    let gen_dir = output_dir.join("generated");
    let inc_dir = output_dir.join("include");
    std::fs::create_dir_all(&gen_dir)?;
    std::fs::create_dir_all(&inc_dir)?;

    // Emit mid-entry wrappers
    let parent_name_map = build_parent_name_map(&prep.analysis.functions);
    let (mid_entries_cpp, mid_entry_fwd_decls) =
        emit_mid_entry_wrappers(&prep.analysis.mid_entries, &parent_name_map);
    tracing::info!("Emitting {} mid-entry wrappers", prep.analysis.mid_entries.len());

    // Progress bar for function decode+emit
    let pb = make_progress_bar(prep.analysis.functions.len() as u64);

    // Silent-path collectors (issue #37): the rayon closure records every
    // decode error and statically-emitted RECOMP_LOOKUP target for the report.
    let decode_errors: Mutex<Vec<DecodeErrorEntry>> = Mutex::new(Vec::new());
    let lookup_targets: Mutex<HashSet<u32>> = Mutex::new(HashSet::new());

    // Batch emit (parallel via rayon inside emit_function_batches)
    let batch_output = emit_function_batches(
        &prep.analysis.functions,
        effective_batch_size,
        &mid_entry_fwd_decls,
        |func| {
            pb.inc(1);
            let (cpp, diag) = emit_one_function(&prep, func);
            if let Some(error) = diag.decode_error {
                decode_errors.lock().unwrap().push(DecodeErrorEntry {
                    address: func.address.clone(),
                    name: func.name.clone(),
                    error,
                });
            }
            if !diag.static_lookup_targets.is_empty() {
                lookup_targets.lock().unwrap().extend(diag.static_lookup_targets);
            }
            cpp
        },
        &prep.unique_names,
    )?;
    pb.finish_with_message("Done decoding functions");

    // Write all output files
    let module_name = &prep.analysis.module_name;
    write_output_files(
        output_dir, &prep.analysis, &batch_output, &mid_entries_cpp, module_name,
        &prep.unique_names,
    )?;

    // Constructors are emitted as RECOMP_LOOKUP calls too (init_array.cpp) —
    // include them in the static dispatch-target audit.
    let mut static_lookup_targets = lookup_targets.into_inner().unwrap();
    static_lookup_targets.extend(prep.analysis.constructors.iter().filter_map(|c| parse_hex_u32(c)));

    // Build + write recompile_report.json (issue #37).
    let recompile_report = report::build_report(report::ReportInputs {
        analysis: &prep.analysis,
        discovery: prep.discovery,
        batch_file_count: batch_output.cpp_files.len(),
        decode_errors: decode_errors.into_inner().unwrap(),
        static_lookup_targets,
        unique_names: &prep.unique_names,
    });
    let report_path = output_dir.join("recompile_report.json");
    std::fs::write(&report_path, serde_json::to_string_pretty(&recompile_report)?)
        .with_context(|| format!("Failed to write {}", report_path.display()))?;

    tracing::info!(
        "Recompile complete: {} functions, {} mid-entries, {} .cpp files -> {}",
        prep.analysis.functions.len(),
        prep.analysis.mid_entries.len(),
        batch_output.cpp_files.len(),
        output_dir.display(),
    );
    println!("{}", report::human_summary(&recompile_report, &report_path));

    // Self-checked counts (issue #37): turn the documented baseline into an
    // assertion when the caller passes --expect-functions/--expect-mid-entries.
    report::check_expectation("functions", prep.analysis.functions.len(), opts.expect_functions)?;
    report::check_expectation(
        "mid-entries", prep.analysis.mid_entries.len(), opts.expect_mid_entries,
    )?;
    Ok(())
}

// -------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------

/// Extract (start_vaddr, end_vaddr) range from decoded segment bytes.
fn segment_range(segment_bytes: &[(u32, Vec<u8>)]) -> (u32, u32) {
    let mut start = u32::MAX;
    let mut end = 0u32;
    for (vaddr, bytes) in segment_bytes {
        start = start.min(*vaddr);
        end = end.max(vaddr + bytes.len() as u32);
    }
    (start, end)
}

/// Discover new function entry points via three complementary binary scans.
///
/// Runs RAW_SCAN xref target consumption, MIPS prologue detection, and
/// gap-start enumeration. Creates `JsonFunction` stubs for each discovered
/// address and clamps sizes to next-function boundaries.
///
/// This is an in-memory-only transformation -- analysis.json on disk is NOT
/// modified.
///
/// Returns the per-pass discovery breakdown (carried into the recompile report).
fn enhance_function_discovery(
    analysis: &mut AnalysisJson,
    segment_bytes: &[(u32, Vec<u8>)],
) -> DiscoveryCounts {
    // Fix stale heuristic placeholder sizes (vtable_miss / binary_scan) to their
    // true extent BEFORE the discovery passes run. Otherwise the gap-start and
    // prologue scans treat a truncated body's tail as empty space and inject
    // phantom entries inside it -- which then re-truncate the real function. The
    // constructor-table functions (code-in-.data Ghidra misses) arrive here with
    // a 256-byte placeholder; growing them first makes func_intervals below
    // protect their real bodies.
    let grown_existing =
        crate::hle_entry_scanner::resize_truncated_framed_functions(
            &mut analysis.functions,
            segment_bytes,
        );

    // Build exclusion set: existing functions + mid-entries
    let existing_funcs: HashSet<u32> = analysis
        .functions
        .iter()
        .filter_map(|f| parse_hex_u32(&f.address))
        .collect();

    let mid_addrs: HashSet<u32> = analysis
        .mid_entries
        .iter()
        .filter_map(|me| parse_hex_u32(&me.addr))
        .collect();

    let all_known: HashSet<u32> = existing_funcs
        .union(&mid_addrs)
        .copied()
        .collect();

    // Build sorted interval array from existing functions for overlap check.
    // An address is "inside" an existing function if start < addr < start+size
    // (strictly inside, not at the start boundary which is already in all_known).
    let mut func_intervals: Vec<(u32, u32)> = analysis
        .functions
        .iter()
        .filter_map(|f| {
            let start = parse_hex_u32(&f.address)?;
            let end = start.checked_add(f.size as u32)?;
            Some((start, end))
        })
        .collect();
    func_intervals.sort_by_key(|&(s, _)| s);

    let mut discovered: HashSet<u32> = HashSet::new();
    let (seg_start, seg_end) = segment_range(segment_bytes);

    // Force-inject addresses that Ghidra merged into larger functions.
    // These are confirmed valid function starts via PPSSPP behavioral oracle
    // but fail is_inside_function because Ghidra's function boundary is too wide.
    const FORCE_ENTRIES: &[u32] = &[
        0x0887E6C0, // GE display list builder; blocks all PRIM submission (quick-19)
    ];

    // D2 (19G/19H): vtable-adapter thunks in the Ghidra gap 0x088273FC..0x08827480.
    // No xref of any kind; reached only by `j 0x08827470` inside the
    // RAW_SCAN-recovered FUN_08858C48 (and `j 0x0882744C` inside FUN_08858C40).
    // Each is a tiny `lw t9,0(a0); lw t9,0x34(t9); jr t9` vtable+0x34 dispatch.
    // Without a function entry they LOOKUP_MISS -> noop_stub, dropping the
    // loadinggroup inflate-destination allocator dispatch — which keeps the
    // BROKEN decompressor at 0x089D7xxx from running. Recovering them makes it
    // run and stall at the D3 inflate-heap wall (regression below D1 baseline),
    // so they are gated behind the same env as the cross-function pass and only
    // active together with D3a. (See the cross-function pass comment + 19H.)
    const FORCE_ENTRIES_D2: &[u32] = &[
        0x08827470, // no-arg vtable+0x34 dispatch (j-target of FUN_08858C48)
        0x0882744C, // with-args variant (j-target of FUN_08858C40)
    ];
    let cross_mid_on = std::env::var("PSPRECOMP_CROSS_MID").as_deref() == Ok("1");

    // Pass 0: Force entries (bypass is_inside_function for known-critical addresses)
    let force_iter = FORCE_ENTRIES.iter().chain(
        if cross_mid_on { FORCE_ENTRIES_D2.iter() } else { [].iter() },
    );
    for &addr in force_iter {
        if addr >= seg_start
            && addr < seg_end
            && addr % 4 == 0
            && !all_known.contains(&addr)
        {
            discovered.insert(addr);
        }
    }
    let force_count = discovered.len();

    // Pass 1: RAW_SCAN xref targets
    for xref in &analysis.xrefs {
        if xref.ref_type != "RAW_SCAN" {
            continue;
        }
        if let Some(to) = parse_hex_u32(&xref.to_addr) {
            if to >= seg_start
                && to < seg_end
                && to % 4 == 0
                && !all_known.contains(&to)
                && !is_inside_function(to, &func_intervals)
            {
                discovered.insert(to);
            }
        }
    }
    let raw_scan_count = discovered.len() - force_count;

    // Pass 2: Prologue scan (ADDIU SP,SP,-N)
    let pre_prologue = discovered.len();
    for (seg_vaddr, bytes) in segment_bytes {
        let word_count = bytes.len() / 4;
        for i in 0..word_count {
            let off = i * 4;
            let word = u32::from_le_bytes(
                bytes[off..off + 4].try_into().unwrap(),
            );
            // ADDIU $sp, $sp, -N: opcode=0x09, rs=29, rt=29 => 0x27BD
            // negative immediate: bit 15 set => (word & 0xFFFF) >= 0x8000
            if (word >> 16) == 0x27BD && (word & 0xFFFF) >= 0x8000 {
                let addr = seg_vaddr + off as u32;
                if !all_known.contains(&addr)
                    && !is_inside_function(addr, &func_intervals)
                {
                    discovered.insert(addr);
                }
            }
        }
    }
    let prologue_count = discovered.len() - pre_prologue;

    // Pass 3: Gap-start enumeration
    // Build sorted list of (addr, size) from existing functions only
    // (discovered entries don't have sizes yet)
    let pre_gap = discovered.len();
    let mut sorted_funcs: Vec<(u32, u64)> = analysis
        .functions
        .iter()
        .filter_map(|f| {
            let addr = parse_hex_u32(&f.address)?;
            Some((addr, f.size))
        })
        .collect();
    sorted_funcs.sort_by_key(|&(a, _)| a);

    for window in sorted_funcs.windows(2) {
        let end_curr = window[0].0 + window[0].1 as u32;
        let start_next = window[1].0;
        if start_next > end_curr
            && end_curr >= seg_start
            && end_curr < seg_end
            && end_curr % 4 == 0
            && !all_known.contains(&end_curr)
            && !is_inside_function(end_curr, &func_intervals)
        {
            discovered.insert(end_curr);
        }
    }
    let gap_count = discovered.len() - pre_gap;

    // Create JsonFunction entries for each discovered address
    for &addr in &discovered {
        analysis.functions.push(JsonFunction {
            name: format!("FUN_{:08X}", addr),
            address: format!("0x{:08X}", addr),
            size: 256, // placeholder; clamped below
            is_external: false,
            is_thunk: false,
            source: "binary_scan".to_string(),
        });
    }

    // Resize the newly-discovered (binary_scan) functions to their true extent
    // too -- they were created with the 256-byte placeholder above. The earlier
    // pre-discovery resize only covered functions present before discovery.
    let grown_new =
        crate::hle_entry_scanner::resize_truncated_framed_functions(
            &mut analysis.functions,
            segment_bytes,
        );

    // Then clamp any residual oversize (incl. Ghidra functions) to the next
    // function boundary.
    crate::hle_entry_scanner::correct_function_sizes(
        &mut analysis.functions,
    );

    // Pass 4 (issue #13): rescue static branch/jump targets that land in
    // unclaimed gaps. The emitter lowers such targets to RECOMP_LOOKUP; with
    // no owner they hit noop_stub, dropping the transfer and corrupting the
    // caller. Iterated to a fixpoint because a rescued tail can itself
    // branch into a further gap; each round is re-sized + re-clamped so the
    // next round sees correct intervals.
    let mut rescued_total = 0usize;
    for _ in 0..4 {
        let rescued = crate::hle_entry_scanner::rescue_gap_branch_targets(
            &mut analysis.functions,
            segment_bytes,
            seg_start,
            seg_end,
        );
        if rescued.is_empty() {
            break;
        }
        rescued_total += rescued.len();
        crate::hle_entry_scanner::resize_truncated_framed_functions(
            &mut analysis.functions,
            segment_bytes,
        );
        crate::hle_entry_scanner::correct_function_sizes(
            &mut analysis.functions,
        );
    }
    tracing::info!(
        "Gap-target rescue: {} unclaimed static branch/jump targets \
         claimed as recovered functions",
        rescued_total,
    );

    tracing::info!(
        "Resized heuristic functions to gap-fill sizes \
         (pre-discovery={}, post-discovery={})",
        grown_existing.len(),
        grown_new.len()
    );

    tracing::info!(
        "Enhanced function discovery: {} new entries \
         (force={}, raw_scan={}, prologue={}, gap_start={})",
        discovered.len(),
        force_count,
        raw_scan_count,
        prologue_count,
        gap_count,
    );

    DiscoveryCounts {
        force: force_count,
        raw_scan: raw_scan_count,
        prologue: prologue_count,
        gap_start: gap_count,
        gap_rescued: rescued_total,
    }
}

/// Force-inject mid-entry addresses that Ghidra's analysis missed but the
/// runtime observes as repeated `LOOKUP_MISS` hits. Each pair is
/// `(mid_entry_addr, parent_addr)`. The parent must already exist in
/// `analysis.functions`; otherwise the entry is silently skipped. Existing
/// `mid_entries` with the same address are not duplicated.
fn inject_force_mid_entries(analysis: &mut AnalysisJson) {
    const FORCE_MID_ENTRIES: &[(u32, u32)] = &[
        // Mid-entry inside FUN_08827E7C, tail-called from FUN_08827F7C via
        // RECOMP_LOOKUP(0x08827F44). 121 hits/run observed pre-fix.
        (0x08827F44, 0x08827E7C),
        // Vtable-referenced mid-entries inside FUN_08827E7C (the thread-lock
        // acquire/release helper reached through FUN_0895B410's object vtable).
        // analysis.json records DATA xrefs for both — they are stored as
        // function pointers in the vtable at 0x08A44480 (slot +12 -> 0x08827EA0,
        // slot +48 -> 0x08827EB0) — but the older analysis pass left them out of
        // `mid_entries`. Without the mid-entry stub, an indirect vtable call to
        // 0x08827EA0 LOOKUP_MISSes -> noop_stub: the helper body never runs, the
        // caller's stack frame is left unbalanced (48-byte leak observed), and
        // the callee-saved registers r16/r17 the caller restores from its frame
        // come back corrupted. That corruption lands in FUN_0885FE90's state-2
        // body: r16 (the asset manager) and r17 (the asset object) are garbage
        // when it calls the by-name resolver FUN_088623E0, so every loadinggroup
        // asset request (systemdata/systemlocalizedata/LogoData/titledata) fails
        // and boot stalls before any draw. (D1, doc 19F.)
        (0x08827EA0, 0x08827E7C),
        (0x08827EB0, 0x08827E7C),
        // Mid-entry 0x08827F9C inside FUN_08827F7C. The coalesce pass merges
        // FUN_08827F7C into its owner FUN_08827E7C; the existing-mid re-point
        // there carries this parent to the owner, so the owner's prologue switch
        // gains `case 0x08827F9C`. The runtime band-aid hle_mid_08827F9C must
        // therefore call FUN_08827E7C (see psp_hle_kernel_memory.cpp). Without
        // this entry the band-aid's entry_point=0x08827F9C falls through.
        (0x08827F9C, 0x08827F7C),
    ];

    let existing: HashSet<u32> = analysis
        .mid_entries
        .iter()
        .filter_map(|me| parse_hex_u32(&me.addr))
        .collect();

    let parents: HashSet<u32> = analysis
        .functions
        .iter()
        .filter_map(|f| parse_hex_u32(&f.address))
        .collect();

    let mut injected = 0usize;
    for &(entry, parent) in FORCE_MID_ENTRIES {
        if existing.contains(&entry) {
            continue;
        }
        if !parents.contains(&parent) {
            tracing::warn!(
                "FORCE_MID_ENTRIES: parent 0x{:08X} not in functions list; \
                 skipping mid-entry 0x{:08X}",
                parent,
                entry,
            );
            continue;
        }
        analysis.mid_entries.push(JsonMidEntry {
            addr: format!("0x{:08X}", entry),
            parent_addr: format!("0x{:08X}", parent),
        });
        injected += 1;
    }

    if injected > 0 {
        tracing::info!(
            "Force-injected {} mid-entries (total mid_entries={})",
            injected,
            analysis.mid_entries.len(),
        );
    }
}

/// Return the absolute static target of a control-transfer op, if it has one.
///
/// Covers the same J/JAL/branch ops the emitter turns into a `goto` (in-range)
/// or a `RECOMP_LOOKUP` (out-of-range). Register-indirect (`Jr`/`Jalr`) and
/// jump-table ops have no single static target and return `None`.
fn op_static_target(op: &MipsOp) -> Option<u32> {
    match *op {
        MipsOp::J { target }
        | MipsOp::Jal { target }
        | MipsOp::Beq { target, .. }
        | MipsOp::Bne { target, .. }
        | MipsOp::Blez { target, .. }
        | MipsOp::Bgtz { target, .. }
        | MipsOp::Bltz { target, .. }
        | MipsOp::Bgez { target, .. }
        | MipsOp::Bltzal { target, .. }
        | MipsOp::Bgezal { target, .. }
        | MipsOp::Bc1t { target, .. }
        | MipsOp::Bc1f { target, .. }
        | MipsOp::VfpuBvf { target, .. }
        | MipsOp::VfpuBvt { target, .. } => Some(target),
        // Hazard-fused branch (issue #12): the static target lives on the
        // wrapped branch — unwrap so coalesce connectivity / mid-entry
        // recovery still see it.
        MipsOp::BranchHazardDelay { ref branch, .. } => op_static_target(branch),
        _ => None,
    }
}

/// Systematic cross-function mid-jump recovery (D3a, 19G/19H).
///
/// Re-decodes every function with the emitter's exact `decode_function` +
/// `optimize` pipeline, extracts each control-transfer's static target, and
/// keeps targets that land STRICTLY inside a *different* function (i.e. not at
/// that function's start, which is already a normal entry) and are not already
/// a function entry or mid-entry. Each such target is registered as a mid-entry
/// of its owning function so the emitter creates a `_entry` wrapper + dispatch
/// entry + prologue case instead of emitting an unbacked `RECOMP_LOOKUP` that
/// falls through to `noop_stub`.
///
/// Using the decoder (not a naive every-word scan) avoids false positives from
/// data words that happen to look like branches. Safe at scale because the
/// mid-entry dispatch mechanism is re-entrant (see `emit_function`, 19H).
fn inject_cross_function_mid_jumps(
    analysis: &mut AnalysisJson,
    segment_bytes: &[(u32, Vec<u8>)],
    data_xrefs: &[(u32, u32)],
) {
    // Sorted (start, end) intervals of all real functions, for owning-function
    // lookup. end = start + size.
    let mut intervals: Vec<(u32, u32)> = analysis
        .functions
        .iter()
        .filter_map(|f| {
            let start = parse_hex_u32(&f.address)?;
            let end = start.checked_add(f.size as u32)?;
            Some((start, end))
        })
        .collect();
    intervals.sort_by_key(|&(s, _)| s);

    // Addresses already covered: function entries + existing mid-entries.
    let entries: HashSet<u32> = analysis
        .functions
        .iter()
        .filter_map(|f| parse_hex_u32(&f.address))
        .collect();
    let mut known_mids: HashSet<u32> = analysis
        .mid_entries
        .iter()
        .filter_map(|me| parse_hex_u32(&me.addr))
        .collect();

    // Find the function strictly containing `addr` (start < addr < end). Returns
    // (start, end) of the owner, or None if addr is a start boundary / unowned.
    let owner_of = |addr: u32| -> Option<(u32, u32)> {
        let idx = intervals.partition_point(|&(s, _)| s <= addr);
        if idx > 0 {
            let (s, e) = intervals[idx - 1];
            if addr > s && addr < e {
                return Some((s, e));
            }
        }
        None
    };

    // Collect (mid_addr, parent_addr) discoveries; dedup via known_mids.
    let mut new_mids: Vec<(u32, u32)> = Vec::new();
    let cfg = OptimizerConfig::default();

    for f in &analysis.functions {
        let Some(fstart) = parse_hex_u32(&f.address) else { continue };
        let fsize = f.size as u32;
        let fend = fstart.saturating_add(fsize);
        let Some(bytes) = get_func_bytes(segment_bytes, fstart, fsize) else { continue };
        let Ok(ops) = psp_decoder::decode_function(bytes, fstart, data_xrefs) else { continue };
        let ops = optimize(ops, &cfg);

        for op in &ops {
            let Some(target) = op_static_target(op) else { continue };
            // In-range targets become C++ gotos — never a LOOKUP_MISS.
            if target >= fstart && target < fend {
                continue;
            }
            // Already a normal entry or known mid-entry — backed by dispatch.
            if entries.contains(&target) || known_mids.contains(&target) {
                continue;
            }
            // Must land strictly inside another function to be a valid mid-entry.
            let Some((owner_start, _owner_end)) = owner_of(target) else { continue };
            if known_mids.insert(target) {
                new_mids.push((target, owner_start));
            }
        }
    }

    for &(mid, parent) in &new_mids {
        analysis.mid_entries.push(JsonMidEntry {
            addr: format!("0x{:08X}", mid),
            parent_addr: format!("0x{:08X}", parent),
        });
    }

    tracing::info!(
        "Cross-function mid-jump recovery: registered {} new mid-entries \
         (total mid_entries={})",
        new_mids.len(),
        analysis.mid_entries.len(),
    );
}

/// Frame signature of a function, used by the coalesce pass.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum FrameSig {
    /// First sp-adjust is `addiu sp, sp, -N` before any `jr` — a real frame owner.
    Owner,
    /// No `addiu sp, sp, -N` prologue before the first `jr` — a continuation /
    /// leaf piece with no own frame (a coalesce-absorption candidate).
    NoPrologue,
}

/// Classify a function's frame signature by decoding its bytes.
///
/// Walks the decoded op stream: returns `Owner` if the first sp-adjust seen is a
/// negative `addiu sp,sp,-N` (frame prologue) occurring before any `jr`;
/// otherwise `NoPrologue`. Returns `None` if bytes are unavailable / decode
/// fails (treated conservatively as not-absorbable by the caller).
fn func_frame_signature(
    segment_bytes: &[(u32, Vec<u8>)],
    data_xrefs: &[(u32, u32)],
    fstart: u32,
    fsize: u32,
) -> Option<FrameSig> {
    let bytes = get_func_bytes(segment_bytes, fstart, fsize)?;
    let ops = psp_decoder::decode_function(bytes, fstart, data_xrefs).ok()?;
    let cfg = OptimizerConfig::default();
    let ops = optimize(ops, &cfg);
    for op in &ops {
        match op {
            MipsOp::Addiu { rt, rs, imm }
                if *rt == psp_ir::Reg::Gpr(29)
                    && *rs == psp_ir::Reg::Gpr(29)
                    && *imm < 0 =>
            {
                return Some(FrameSig::Owner);
            }
            MipsOp::Addi { rt, rs, imm }
                if *rt == psp_ir::Reg::Gpr(29)
                    && *rs == psp_ir::Reg::Gpr(29)
                    && *imm < 0 =>
            {
                return Some(FrameSig::Owner);
            }
            MipsOp::Jr { .. } => return Some(FrameSig::NoPrologue),
            // A hazard-fused `jr` still ends the piece (issue #12).
            MipsOp::BranchHazardDelay { branch, .. }
                if matches!(branch.as_ref(), MipsOp::Jr { .. }) =>
            {
                return Some(FrameSig::NoPrologue);
            }
            _ => {}
        }
    }
    Some(FrameSig::NoPrologue)
}

/// Return every DIRECT static control-flow target of a function (decoded).
///
/// Used by the coalesce pass for the owner's `edge_in` connectivity test: a
/// sibling is only absorbed if the OWNER directly targets it (not transitively).
fn func_direct_targets(
    segment_bytes: &[(u32, Vec<u8>)],
    data_xrefs: &[(u32, u32)],
    fstart: u32,
    fsize: u32,
) -> HashSet<u32> {
    let mut targets = HashSet::new();
    let Some(bytes) = get_func_bytes(segment_bytes, fstart, fsize) else { return targets };
    let Ok(ops) = psp_decoder::decode_function(bytes, fstart, data_xrefs) else { return targets };
    let ops = optimize(ops, &OptimizerConfig::default());
    for op in &ops {
        if let Some(t) = op_static_target(&op_unwrap_ds(op)) {
            targets.insert(t);
        }
    }
    targets
}

/// Unwrap a `DelaySlot` wrapper to inspect the inner op (branch-likely DS).
fn op_unwrap_ds(op: &MipsOp) -> MipsOp {
    match op {
        MipsOp::DelaySlot { instr } => (**instr).clone(),
        other => other.clone(),
    }
}

/// Build a map: target address -> set of source-function-start addresses that
/// statically branch/jump/call to it.
///
/// Decodes every function once. The "no external entry" guard in
/// `coalesce_split_frame_siblings` uses this to absorb a sibling ONLY when every
/// incoming static edge originates inside the owning cluster.
fn build_incoming_edges(
    functions: &[JsonFunction],
    segment_bytes: &[(u32, Vec<u8>)],
    data_xrefs: &[(u32, u32)],
) -> HashMap<u32, HashSet<u32>> {
    let mut incoming: HashMap<u32, HashSet<u32>> = HashMap::new();
    for f in functions {
        let Some(fstart) = parse_hex_u32(&f.address) else { continue };
        let fsize = f.size as u32;
        let Some(bytes) = get_func_bytes(segment_bytes, fstart, fsize) else { continue };
        let Ok(ops) = psp_decoder::decode_function(bytes, fstart, data_xrefs) else { continue };
        let ops = optimize(ops, &OptimizerConfig::default());
        for op in &ops {
            if let Some(t) = op_static_target(&op_unwrap_ds(op)) {
                incoming.entry(t).or_default().insert(fstart);
            }
        }
    }
    incoming
}

/// Coalesce Ghidra-over-split shared-frame siblings into single emitted C++
/// functions.
///
/// A **frame owner** is a function whose first sp-adjust is `addiu sp,sp,-N`
/// (a real prologue). The pass greedily absorbs the run of immediately-contiguous
/// (`sib_start == cluster_end`) following functions that have NO own prologue,
/// but ONLY when a sibling is reached EXCLUSIVELY from within the growing cluster.
///
/// Absorption guard (the strict "no external entry" rule — doc-29 §NEXT STEP):
/// a contiguous no-prologue sibling is absorbed iff ALL hold:
///   1. it is connected by a DIRECT static edge from the OWNER (owner targets the
///      sibling start), OR the sibling falls strictly inside the owner's body via
///      contiguity (a pure fallthrough continuation, no incoming edge at all);
///   2. every incoming static edge to the sibling start originates inside
///      `[owner_start, cluster_end)` (no external call/branch source);
///   3. the sibling start is NOT a DATA-xref target (vtable / function-pointer
///      table — an indirect dispatch entry that must stay a standalone function);
///   4. the sibling start is NOT an existing mid_entry of a DIFFERENT parent.
/// A sibling failing ANY guard stops the cluster (remaining contiguous functions
/// are not absorbed by this owner). This keeps externally-reachable functions
/// standalone, matching doc-27's ~394 owner count.
///
/// On absorb: grow the owner `size` to span the cluster, remove the absorbed
/// siblings from `analysis.functions`, register each absorbed sibling start as a
/// mid-entry of the owner, and re-point any pre-existing mid-entry whose parent
/// was absorbed. Returns the set of owner addresses that absorbed >=1 sibling.
fn coalesce_split_frame_siblings(
    analysis: &mut AnalysisJson,
    segment_bytes: &[(u32, Vec<u8>)],
    data_xrefs: &[(u32, u32)],
) -> HashSet<u32> {
    // Sorted (start, size) of every function.
    let mut funcs: Vec<(u32, u32)> = analysis
        .functions
        .iter()
        .filter_map(|f| Some((parse_hex_u32(&f.address)?, f.size as u32)))
        .collect();
    funcs.sort_by_key(|&(s, _)| s);

    // start -> size lookup.
    let size_of: HashMap<u32, u32> = funcs.iter().copied().collect();

    // Incoming static-edge map and DATA-xref target set (external dispatch entries).
    let incoming = build_incoming_edges(&analysis.functions, segment_bytes, data_xrefs);
    let data_targets: HashSet<u32> = data_xrefs.iter().map(|&(_, to)| to).collect();

    // Existing mid-entry -> parent map (to re-point after absorption / guard #4).
    let mid_parent: HashMap<u32, u32> = analysis
        .mid_entries
        .iter()
        .filter_map(|me| Some((parse_hex_u32(&me.addr)?, parse_hex_u32(&me.parent_addr)?)))
        .collect();

    // Cache: function start -> FrameSig.
    let mut sig_cache: HashMap<u32, FrameSig> = HashMap::new();
    let mut sig_of = |start: u32, sz: u32| -> FrameSig {
        *sig_cache
            .entry(start)
            .or_insert_with(|| {
                func_frame_signature(segment_bytes, data_xrefs, start, sz)
                    .unwrap_or(FrameSig::Owner)
            })
    };

    let mut owners: HashSet<u32> = HashSet::new();
    // owner_start -> (new_size, Vec<absorbed_sibling_start>)
    let mut absorbed: HashMap<u32, (u32, Vec<u32>)> = HashMap::new();
    // siblings removed from the function list (any owner).
    let mut removed: HashSet<u32> = HashSet::new();

    let mut idx = 0usize;
    while idx < funcs.len() {
        let (owner_start, owner_size) = funcs[idx];
        if removed.contains(&owner_start) {
            idx += 1;
            continue;
        }
        if sig_of(owner_start, owner_size) != FrameSig::Owner {
            idx += 1;
            continue;
        }

        // The owner's DIRECT static targets (for the edge_in connectivity test).
        let owner_targets =
            func_direct_targets(segment_bytes, data_xrefs, owner_start, owner_size);

        // Phase 1: determine the MAXIMAL contiguous candidate run of no-prologue
        // siblings. Stop only at a structural boundary (non-contiguous, has own
        // prologue, a DATA/vtable dispatch target, or a mid-entry of a different
        // parent). The external-entry guard below is evaluated against this FULL
        // candidate range so a legitimate intra-cluster forward reference (e.g. a
        // sibling jal'd from a LATER absorbed piece) is not misread as external.
        let mut cand_end = owner_start.saturating_add(owner_size);
        let mut candidates: Vec<(u32, u32)> = Vec::new();
        let mut j = idx + 1;
        while j < funcs.len() {
            let (sib_start, sib_size) = funcs[j];
            if sib_start != cand_end {
                break;
            }
            if sig_of(sib_start, sib_size) != FrameSig::NoPrologue {
                break;
            }
            if data_targets.contains(&sib_start) {
                break;
            }
            if let Some(&p) = mid_parent.get(&sib_start) {
                if p != owner_start {
                    break;
                }
            }
            candidates.push((sib_start, sib_size));
            cand_end = sib_start.saturating_add(sib_size);
            j += 1;
        }

        // Phase 2: absorb the leading prefix of the candidate run, truncating at
        // the first sibling reached from OUTSIDE the full candidate range. The
        // "no external entry" guard: every incoming static edge to a sibling
        // start must originate inside [owner_start, cand_end). A sibling with an
        // external call/branch source MUST remain a standalone function — so it
        // and everything after it stay separate.
        let mut cluster_end = owner_start.saturating_add(owner_size);
        let mut cluster_members: Vec<u32> = Vec::new();
        for (sib_start, sib_size) in candidates {
            // Guard #2: no incoming edge from outside the candidate cluster.
            let external_entry = incoming
                .get(&sib_start)
                .map(|srcs| srcs.iter().any(|&src| src < owner_start || src >= cand_end))
                .unwrap_or(false);
            if external_entry {
                break;
            }
            // Guard #1: connected by a DIRECT edge from the owner, OR a pure
            // fallthrough continuation (no incoming edge at all), OR reached
            // exclusively from within the candidate cluster. All three mean the
            // sibling is part of the shared frame, not a standalone entry.
            let owner_edge = owner_targets.contains(&sib_start);
            let incoming_srcs = incoming.get(&sib_start);
            let no_incoming = incoming_srcs.map(|s| s.is_empty()).unwrap_or(true);
            let intra_cluster_only = incoming_srcs
                .map(|srcs| {
                    !srcs.is_empty()
                        && srcs.iter().all(|&src| src >= owner_start && src < cand_end)
                })
                .unwrap_or(false);
            if !(owner_edge || no_incoming || intra_cluster_only) {
                break;
            }

            cluster_members.push(sib_start);
            cluster_end = sib_start.saturating_add(sib_size);
        }

        if !cluster_members.is_empty() {
            if std::env::var("PSPRECOMP_COALESCE_DEBUG").as_deref() == Ok("1")
                && cluster_members.len() >= 4
            {
                tracing::info!(
                    "COALESCE owner 0x{:08X} absorbed {} siblings: {}",
                    owner_start,
                    cluster_members.len(),
                    cluster_members
                        .iter()
                        .map(|m| format!("0x{:08X}", m))
                        .collect::<Vec<_>>()
                        .join(","),
                );
            }
            owners.insert(owner_start);
            let new_size = cluster_end.saturating_sub(owner_start);
            for &m in &cluster_members {
                removed.insert(m);
            }
            absorbed.insert(owner_start, (new_size, cluster_members));
        }
        idx = j.max(idx + 1);
    }

    // Apply: grow owner sizes, register absorbed siblings as mid-entries,
    // re-point pre-existing mid-entries whose parent was absorbed, and remove
    // absorbed siblings from the function list.
    let removed_to_owner: HashMap<u32, u32> = absorbed
        .iter()
        .flat_map(|(&owner, (_, members))| members.iter().map(move |&m| (m, owner)))
        .collect();

    // Grow owner sizes.
    for f in &mut analysis.functions {
        if let Some(start) = parse_hex_u32(&f.address) {
            if let Some((new_size, _)) = absorbed.get(&start) {
                f.size = *new_size as u64;
            }
        }
    }
    // Drop absorbed siblings.
    analysis
        .functions
        .retain(|f| parse_hex_u32(&f.address).map(|a| !removed.contains(&a)).unwrap_or(true));

    // Re-point existing mid-entries whose parent was absorbed -> the owner.
    for me in &mut analysis.mid_entries {
        if let Some(parent) = parse_hex_u32(&me.parent_addr) {
            if let Some(&owner) = removed_to_owner.get(&parent) {
                me.parent_addr = format!("0x{:08X}", owner);
            }
        }
    }
    // Register each absorbed sibling start as a mid-entry of its owner (dedup).
    let existing_mids: HashSet<u32> = analysis
        .mid_entries
        .iter()
        .filter_map(|me| parse_hex_u32(&me.addr))
        .collect();
    let mut new_mid_count = 0usize;
    for (&owner, (_, members)) in &absorbed {
        for &m in members {
            if existing_mids.contains(&m) {
                continue;
            }
            analysis.mid_entries.push(JsonMidEntry {
                addr: format!("0x{:08X}", m),
                parent_addr: format!("0x{:08X}", owner),
            });
            new_mid_count += 1;
        }
    }

    let total_siblings: usize = absorbed.values().map(|(_, m)| m.len()).sum();
    let _ = &size_of;
    tracing::info!(
        "Coalesce: {} siblings -> {} frame owners ({} new mid-entries, total mid_entries={})",
        total_siblings,
        owners.len(),
        new_mid_count,
        analysis.mid_entries.len(),
    );

    owners
}

/// Read and deserialize analysis.json.
fn load_analysis(path: &Path) -> anyhow::Result<AnalysisJson> {
    let text = std::fs::read_to_string(path)
        .with_context(|| format!("Cannot read {}", path.display()))?;
    serde_json::from_str(&text)
        .with_context(|| format!("Invalid JSON in {}", path.display()))
}

/// Decode all segment data_b64 fields into (p_vaddr, raw_bytes) pairs.
fn decode_segment_bytes(analysis: &AnalysisJson) -> Vec<(u32, Vec<u8>)> {
    analysis.segments.iter().filter_map(|seg| {
        let vaddr = parse_hex_u32(&seg.p_vaddr)?;
        let bytes = B64.decode(&seg.data_b64).ok()?;
        Some((vaddr, bytes))
    }).collect()
}

/// Slice raw bytes for a function from decoded segments.
fn get_func_bytes<'a>(
    segments: &'a [(u32, Vec<u8>)],
    func_vaddr: u32,
    func_size: u32,
) -> Option<&'a [u8]> {
    segments.iter().find_map(|(seg_vaddr, bytes)| {
        let end = seg_vaddr.checked_add(bytes.len() as u32)?;
        if func_vaddr >= *seg_vaddr && func_vaddr + func_size <= end {
            let off = (func_vaddr - seg_vaddr) as usize;
            Some(&bytes[off..off + func_size as usize])
        } else {
            None
        }
    })
}

/// Per-function emission diagnostics, consumed by the recompile report.
#[derive(Debug, Default)]
pub struct EmitDiagnostics {
    /// Set when the function was stubbed because the decoder errored.
    pub decode_error: Option<String>,
    /// Statically-known cross-function RECOMP_LOOKUP targets emitted.
    pub static_lookup_targets: Vec<u32>,
}

/// Decode and emit a single function to a C++ string, using an explicit (possibly deduplicated) name.
///
/// Returns a stub body if bytes are unavailable or if the function is in
/// config.skips / config.stubs. Applies config.patches before decode.
/// The second tuple element carries silent-path diagnostics for the report.
pub fn decode_and_emit_function_with_name(
    func: &JsonFunction,
    canonical_name: &str,
    segments: &[(u32, Vec<u8>)],
    import_map: &HashMap<u32, String>,
    _func_map: &HashMap<u32, String>,
    config: &GameConfig,
    data_xrefs: &[(u32, u32)],
    mid_entry_addr_map: &HashMap<u32, Vec<u32>>,
    coalesced: bool,
) -> (String, EmitDiagnostics) {
    let mut diag = EmitDiagnostics::default();
    let func_vaddr = match parse_hex_u32(&func.address) {
        Some(v) => v,
        None => return (make_stub_body(canonical_name, "// invalid address"), diag),
    };
    let func_size = func.size as u32;
    let cpp_name = psp_emitter::sanitize_identifier(canonical_name);

    // Check config.skips by address
    if config.skips.iter().any(|s| s.address == func.address) {
        return (make_stub_body(&cpp_name, "// skipped per game config"), diag);
    }
    // Check config.stubs by name
    if config.stubs.iter().any(|s| s.name == func.name) {
        return (make_stub_body(&cpp_name, "// HLE stub (name match in game config)"), diag);
    }

    let raw_bytes = match get_func_bytes(segments, func_vaddr, func_size) {
        Some(b) => b,
        None => return (make_stub_body(&cpp_name, "// bytes unavailable"), diag),
    };

    // Apply patches: build mutable copy and overwrite patched words
    let mut patched = raw_bytes.to_vec();
    apply_patches(&mut patched, func_vaddr, &config.patches);

    let ops = match psp_decoder::decode_function(&patched, func_vaddr, data_xrefs) {
        Ok(ops) => ops,
        Err(e) => {
            tracing::warn!("Decode error in {} @ 0x{:08X}: {e}", func.name, func_vaddr);
            diag.decode_error = Some(e.to_string());
            return (make_stub_body(&cpp_name, &format!("// decode error: {e}")), diag);
        }
    };

    let ops = optimize(ops, &OptimizerConfig::default());

    // Build DecodedFunction for emission
    let decoded = DecodedFunction {
        vaddr: func_vaddr,
        name: func.name.clone(),
        cpp_name: cpp_name.clone(),
        size: func_size,
        blocks: vec![BasicBlock { vaddr: func_vaddr, instrs: ops }],
        is_mid_entry_parent: mid_entry_addr_map.contains_key(&func_vaddr),
        mid_entry_addrs: mid_entry_addr_map.get(&func_vaddr).cloned().unwrap_or_default(),
        coalesced,
    };

    let mut gen = CppGenerator::new();
    psp_emitter::function::emit_function(&decoded, &mut gen, import_map);
    diag.static_lookup_targets = gen.take_static_lookup_targets();
    (gen.take_output(), diag)
}

/// Emit an empty stub function body with a comment.
fn make_stub_body(cpp_name: &str, comment: &str) -> String {
    format!(
        "RECOMP_FUNC void {}(uint8_t* rdram, recomp_context* ctx) {{\n    {comment}\n}}\n",
        cpp_name
    )
}

/// Apply patches to a mutable byte buffer (in-place word overwrite).
fn apply_patches(
    bytes: &mut Vec<u8>,
    base_vaddr: u32,
    patches: &[crate::config::PatchEntry],
) {
    for patch in patches {
        let Some(patch_vaddr) = parse_hex_u32(&patch.address) else { continue };
        let Some(new_word) = parse_hex_u32(&patch.instruction) else { continue };
        if patch_vaddr < base_vaddr { continue; }
        let off = (patch_vaddr - base_vaddr) as usize;
        if off + 4 > bytes.len() { continue; }
        let le_bytes = new_word.to_le_bytes();
        bytes[off..off + 4].copy_from_slice(&le_bytes);
    }
}

/// Build a map from function address → unique C++ name, deduplicating collisions.
///
/// When Ghidra produces multiple functions with the same name (e.g., multiple
/// `thunk_FUN_xxx` entries), appends `_ADDR` suffix to all colliding entries
/// to produce unique C++ identifiers.
pub fn dedup_function_names(functions: &[JsonFunction]) -> HashMap<u32, String> {
    use std::collections::HashMap as Map;
    // First pass: count how many functions share each sanitized name
    let mut name_addrs: Map<String, Vec<u32>> = Map::new();
    for func in functions {
        let addr = parse_hex_u32(&func.address).unwrap_or(0);
        let sanitized = psp_emitter::sanitize_identifier(&func.name);
        name_addrs.entry(sanitized).or_default().push(addr);
    }
    // Second pass: for names with collisions, append _ADDR suffix to each
    let mut result: Map<u32, String> = Map::new();
    for func in functions {
        let addr = parse_hex_u32(&func.address).unwrap_or(0);
        let sanitized = psp_emitter::sanitize_identifier(&func.name);
        let unique_name = if name_addrs.get(&sanitized).map(|v| v.len()).unwrap_or(0) > 1 {
            // Collision: append the function's own address
            format!("{}_{:08x}", sanitized, addr)
        } else {
            func.name.clone()
        };
        result.insert(addr, unique_name);
    }
    result
}

/// Check if `addr` falls strictly inside any existing function interval.
///
/// Uses binary search on a sorted `(start, end)` interval array.
/// Returns true if `start < addr < end` for any interval (i.e., addr is
/// strictly inside -- not at the start boundary, which is handled by
/// `all_known`).
fn is_inside_function(addr: u32, intervals: &[(u32, u32)]) -> bool {
    // Find the rightmost interval whose start <= addr
    let idx = intervals.partition_point(|&(start, _)| start <= addr);
    // If idx > 0, check if addr is strictly inside intervals[idx-1]
    idx > 0 && addr > intervals[idx - 1].0 && addr < intervals[idx - 1].1
}

/// Parse a hex string (with or without "0x" prefix) into u32.
pub(crate) fn parse_hex_u32(s: &str) -> Option<u32> {
    let trimmed = s.trim_start_matches("0x").trim_start_matches("0X");
    u64::from_str_radix(trimmed, 16).ok().map(|v| v as u32)
}

/// Create an indicatif progress bar for N items.
fn make_progress_bar(len: u64) -> ProgressBar {
    let pb = ProgressBar::new(len);
    pb.set_style(
        ProgressStyle::default_bar()
            .template("[{elapsed_precise}] {bar:40} {pos}/{len} Decoding functions...")
            .unwrap_or_else(|_| ProgressStyle::default_bar()),
    );
    pb
}

/// Write all generated output files to disk.
fn write_output_files(
    output_dir: &Path,
    analysis: &AnalysisJson,
    batch_output: &psp_emitter::batch::BatchOutput,
    mid_entries_cpp: &str,
    module_name: &str,
    unique_names: &HashMap<u32, String>,
) -> anyhow::Result<()> {
    // Remove stale batch_*.cpp from a prior run before writing fresh ones.
    // A run that emits fewer batches than the previous one leaves orphaned
    // higher-numbered files behind. Because CMake globs batch_*.cpp, those
    // stale files are compiled+linked, producing duplicate symbols and (worse)
    // resurrecting outdated translations -- e.g. an old over-extended function
    // body full of mis-decoded jump-table words. Clean the slate first.
    remove_stale_batch_files(&output_dir.join("generated"))?;

    // Write batch .cpp files
    for (filename, content) in &batch_output.cpp_files {
        std::fs::write(output_dir.join(filename), content)?;
    }

    // Write support files
    std::fs::write(output_dir.join("funcs.h"), &batch_output.funcs_h)?;
    std::fs::write(output_dir.join("mid_entries.cpp"), mid_entries_cpp)?;
    std::fs::write(
        output_dir.join("dispatch.cpp"),
        emit_dispatch_table(&analysis.functions, &analysis.mid_entries, module_name, unique_names),
    )?;
    std::fs::write(
        output_dir.join("data_sections.cpp"),
        emit_data_sections(&analysis.segments)?,
    )?;
    std::fs::write(
        output_dir.join("init_array.cpp"),
        emit_psp_call_constructors(&analysis.constructors),
    )?;
    std::fs::write(
        output_dir.join("CMakeLists.txt"),
        emit_cmake_lists(module_name),
    )?;
    std::fs::write(
        output_dir.join("include").join("recomp.h"),
        CppGenerator::emit_recomp_h(),
    )?;
    Ok(())
}

/// Delete leftover `batch_*.cpp` files from a previous recompile.
///
/// The number of batches can shrink between runs; orphaned files would
/// otherwise be picked up by the `batch_*.cpp` CMake glob and cause duplicate
/// symbols or stale code. Only files matching the `batch_NNNN.cpp` naming are
/// removed; all other generated artifacts are left untouched. A missing
/// directory is not an error (first run).
fn remove_stale_batch_files(gen_dir: &Path) -> anyhow::Result<()> {
    let entries = match std::fs::read_dir(gen_dir) {
        Ok(e) => e,
        Err(_) => return Ok(()), // dir not created yet -> nothing to clean
    };
    for entry in entries.flatten() {
        let name = entry.file_name();
        let name = name.to_string_lossy();
        if name.starts_with("batch_") && name.ends_with(".cpp") {
            let _ = std::fs::remove_file(entry.path());
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn emit_with_words(words: &[u32]) -> (String, EmitDiagnostics) {
        let base = 0x0880_4000u32;
        let bytes: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
        let func = JsonFunction {
            name: "FUN_08804000".into(),
            address: format!("0x{base:08X}"),
            size: bytes.len() as u64,
            is_external: false,
            is_thunk: false,
            source: "ghidra".into(),
        };
        let segments = vec![(base, bytes)];
        decode_and_emit_function_with_name(
            &func,
            "FUN_08804000",
            &segments,
            &HashMap::new(),
            &HashMap::new(),
            &GameConfig::default(),
            &[],
            &HashMap::new(),
            false,
        )
    }

    #[test]
    fn decode_error_is_surfaced_in_diagnostics() {
        // Opcode 0x10 (COP0) with rs_field 0x10 has no decoding — guaranteed
        // DecodeError::Unknown, the silent stub path issue #37 aggregates.
        let (cpp, diag) = emit_with_words(&[0x4200_0000, 0x0000_0000]);
        let err = diag.decode_error.expect("decoder must report an error");
        assert!(cpp.contains("decode error"), "stub body must carry the error comment");
        assert!(!err.is_empty());
    }

    #[test]
    fn clean_decode_has_no_error_and_records_lookup_targets() {
        // jal 0x08900000 (cross-function) + nop delay slot + jr ra + nop.
        let jal = 0x0C00_0000 | ((0x0890_0000 >> 2) & 0x03FF_FFFF);
        let jr_ra = 0x03E0_0008;
        let (cpp, diag) = emit_with_words(&[jal, 0, jr_ra, 0]);
        assert!(diag.decode_error.is_none());
        assert!(
            diag.static_lookup_targets.contains(&0x0890_0000),
            "cross-function jal must be recorded for the dispatch audit; got {:?}",
            diag.static_lookup_targets,
        );
        assert!(cpp.contains("RECOMP_LOOKUP(0x08900000)"));
    }
}
