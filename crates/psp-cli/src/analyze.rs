//! Full `psprecomp analyze` pipeline: Ghidra invocation, Rust post-processing, mid-entry
//! detection, and analysis.json output.

use anyhow::{bail, Context, Result};
use base64::Engine as _;
use psp_parser::analysis_json::{JsonFunction, JsonMidEntry, JsonXref};
use std::path::Path;
use std::process::Command;

/// Invoke Ghidra headless analysis and write raw JSON to `output`.
///
/// Verifies that the ghidra-allegrex plugin is installed before running.
pub fn run_ghidra_analysis(binary: &Path, ghidra_dir: &Path, output: &Path) -> Result<()> {
    // Verify plugin is installed
    let plugin_dir = ghidra_dir.join("Ghidra/Processors/Allegrex");
    if !plugin_dir.exists() {
        bail!(
            "ghidra-allegrex plugin not found at {}\n\
             Install it: copy the Allegrex/ directory from the ghidra-allegrex ZIP \
             into {}/Ghidra/Processors/",
            plugin_dir.display(),
            ghidra_dir.display()
        );
    }

    // Locate ExtractAnalysis.java relative to the binary
    let script = find_analysis_script()?;
    let script_dir = script
        .parent()
        .context("ExtractAnalysis.java has no parent directory")?
        .to_str()
        .context("script directory path not UTF-8")?
        .to_owned();
    let headless = ghidra_dir.join("support/analyzeHeadless");

    let status = Command::new(&headless)
        .args([
            "/tmp/ghidra_projects",
            "psprecomp_analysis",
            "-import",
            binary.to_str().context("binary path not UTF-8")?,
            "-scriptPath",
            &script_dir,
            "-postScript",
            "ExtractAnalysis.java",
            output.to_str().context("output path not UTF-8")?,
            "-deleteProject",
            "-overwrite",
        ])
        .status()
        .with_context(|| format!("Failed to execute {}", headless.display()))?;

    if !status.success() {
        bail!("Ghidra headless failed with exit code {:?}", status.code());
    }
    Ok(())
}

/// Find ExtractAnalysis.java relative to the executable or in `$PWD/analysis/`.
fn find_analysis_script() -> Result<std::path::PathBuf> {
    let candidates = [
        std::env::current_dir()?.join("analysis/ExtractAnalysis.java"),
        std::env::current_exe()?
            .parent()
            .unwrap_or(Path::new("."))
            .join("../analysis/ExtractAnalysis.java"),
    ];
    for path in &candidates {
        if path.exists() {
            return Ok(path.clone());
        }
    }
    bail!("ExtractAnalysis.java not found; expected at ./analysis/ExtractAnalysis.java");
}

/// Detect mid-function entry points from Ghidra xrefs (V1-compatible approach).
///
/// V1's `_find_midentry_points()` used xrefs from Ghidra's ReferenceManager —
/// DATA references from .data/.rodata pointing to addresses inside known function
/// bodies (but not at function starts). This produces ~532 entries for Patapon BOOT.BIN.
///
/// Branch-scan (previous approach) found 31,042 — too broad for Phase 2.
fn detect_mid_entries_from_xrefs(
    functions: &[JsonFunction],
    xrefs: &[JsonXref],
) -> Vec<JsonMidEntry> {
    use std::collections::HashSet;

    let func_starts: HashSet<u64> = functions
        .iter()
        .filter_map(|f| {
            u64::from_str_radix(f.address.trim_start_matches("0x"), 16).ok()
        })
        .collect();

    let mut sorted_funcs: Vec<(u64, u64, String)> = functions
        .iter()
        .filter_map(|f| {
            let start = u64::from_str_radix(f.address.trim_start_matches("0x"), 16).ok()?;
            Some((start, start + f.size, f.address.clone()))
        })
        .collect();
    sorted_funcs.sort_by_key(|(s, _, _)| *s);

    let mut mid_set: HashSet<u64> = HashSet::new();
    let mut mid_entries: Vec<JsonMidEntry> = Vec::new();

    for xref in xrefs {
        let to_addr = match u64::from_str_radix(
            xref.to_addr.trim_start_matches("0x").trim_start_matches("0X"),
            16,
        ) {
            Ok(a) => a,
            Err(_) => continue,
        };
        if func_starts.contains(&to_addr) {
            continue;
        }

        let pos = sorted_funcs.partition_point(|(s, _, _)| *s <= to_addr);
        if pos == 0 {
            continue;
        }
        let (start, end, parent_addr) = &sorted_funcs[pos - 1];
        if to_addr < *start || to_addr >= *end {
            continue;
        }

        if mid_set.insert(to_addr) {
            mid_entries.push(JsonMidEntry {
                addr: format!("0x{to_addr:08X}"),
                parent_addr: parent_addr.clone(),
            });
        }
    }

    mid_entries
}

/// Run HLE entry point scan: discover function pointers passed to HLE APIs.
///
/// Builds the stub address map from either PRX imports or ELF .lib.stub,
/// scans the .text segment for JAL call sites, and merges new entries into
/// the functions list.
#[allow(clippy::too_many_arguments)]
fn run_hle_entry_scan(
    raw_data: &[u8],
    elf_obj: &goblin::elf::Elf,
    _is_prx: bool,
    nid_map: &std::collections::HashMap<u32, String>,
    import_stubs: &[psp_parser::types::ImportStub],
    segments: &[psp_parser::types::Segment],
    seg_data_vecs: &[Vec<u8>],
    functions: &mut Vec<JsonFunction>,
    _output: &std::path::PathBuf,
) -> Result<crate::hle_entry_scanner::HleDiscoveryResult> {
    use crate::hle_entry_scanner;

    // Build HLE stub address map
    let stub_map = if !import_stubs.is_empty() {
        // PRX: build from analysis.json imports directly
        hle_entry_scanner::build_stub_map_from_imports(import_stubs)
    } else {
        // ELF (like Patapon): discover from .lib.stub section
        hle_entry_scanner::discover_import_stubs_for_elf(
            raw_data, elf_obj, nid_map,
        )
    };

    if stub_map.is_empty() {
        tracing::warn!("No HLE funcptr API stubs found; skipping scan");
        return Ok(hle_entry_scanner::HleDiscoveryResult {
            call_sites_scanned: 0,
            discoveries: Vec::new(),
            new_entries: Vec::new(),
            size_corrections: Vec::new(),
            functions_before: functions.len(),
        });
    }

    // Find .text segment (PF_X = 0x1)
    let text_seg = segments.iter().enumerate().find(|(_, s)| {
        s.p_flags & 0x1 != 0 // PF_X: executable
    });

    let (text_bytes, text_base) = match text_seg {
        Some((idx, seg)) => (&seg_data_vecs[idx][..], seg.p_vaddr),
        None => {
            tracing::warn!("No executable segment found; skipping HLE scan");
            return Ok(hle_entry_scanner::HleDiscoveryResult {
                call_sites_scanned: 0,
                discoveries: Vec::new(),
                new_entries: Vec::new(),
                size_corrections: Vec::new(),
                functions_before: functions.len(),
            });
        }
    };

    // Scan for HLE entry points
    let result = hle_entry_scanner::scan_hle_entries(
        text_bytes, text_base, &stub_map, functions,
    );

    tracing::info!(
        "HLE scan: {} call sites, {} discoveries ({} new), {} known",
        result.call_sites_scanned,
        result.discoveries.len(),
        result.new_entries.len(),
        result.discoveries.len() - result.new_entries.len()
    );

    // Merge new entries into functions list
    for entry in &result.new_entries {
        functions.push(JsonFunction {
            name: format!("FUN_{:08X}", entry.address),
            address: format!("0x{:08X}", entry.address),
            size: entry.estimated_size,
            is_external: false,
            is_thunk: false,
            source: "hle_scan".into(),
        });
    }

    Ok(result)
}

/// Full analyze pipeline: Ghidra + Rust post-processing → analysis.json.
pub fn run_analyze(
    binary: &std::path::PathBuf,
    output: &std::path::PathBuf,
    ghidra_dir: Option<&std::path::PathBuf>,
    nid_db: &std::path::PathBuf,
) -> Result<()> {
    use base64::engine::general_purpose::STANDARD as B64;
    use psp_parser::{elf, imports, nid, prx, reloc};

    tracing::info!("Analyzing {}", binary.display());

    // 1. Read binary
    let raw_data = std::fs::read(binary)
        .with_context(|| format!("Cannot read {}", binary.display()))?;

    // 2. Parse ELF/PRX
    let elf_obj = elf::parse_elf(&raw_data)?;
    let is_prx = prx::is_prx(&elf_obj);
    tracing::info!("Binary type: {}", if is_prx { "PRX" } else { "ELF" });

    // 3. Extract segments (BSS zeroed to p_memsz)
    let segments = elf::extract_segments(&raw_data, &elf_obj);
    let heap_base = elf::calculate_heap_base(&segments);
    tracing::info!("Heap base: 0x{heap_base:08X}");

    // 4. Parse and apply relocations (PRX only)
    let (mut seg_data_vecs, seg_bases): (Vec<Vec<u8>>, Vec<u32>) =
        segments.iter().map(|s| (s.data.clone(), s.p_vaddr)).unzip();
    let mut all_reloc_entries = vec![];

    if is_prx {
        let (type_a_phidxs, type_b_phidxs) = prx::find_reloc_segments(&elf_obj);
        let mut type_a_entries = vec![];
        for idx in type_a_phidxs {
            let ph = &elf_obj.program_headers[idx];
            let raw =
                &raw_data[ph.p_offset as usize..(ph.p_offset + ph.p_filesz) as usize];
            type_a_entries.extend(reloc::parse_type_a_entries(raw)?);
        }
        all_reloc_entries.extend(type_a_entries.iter().cloned());

        let type_b_data: Vec<u8> = type_b_phidxs
            .iter()
            .flat_map(|&idx| {
                let ph = &elf_obj.program_headers[idx];
                raw_data[ph.p_offset as usize..(ph.p_offset + ph.p_filesz) as usize]
                    .to_vec()
            })
            .collect();

        let reloc_stats = reloc::apply_relocations(
            &mut seg_data_vecs,
            &seg_bases,
            &type_a_entries,
            &type_b_data,
        )
        .context("Relocation application failed")?;
        tracing::info!(
            "Applied {} relocations ({} unhandled types — see warnings above)",
            reloc_stats.handled,
            reloc_stats.unhandled.len(),
        );

        // Also collect Type-B entries for the JSON record
        if !type_b_data.is_empty() {
            all_reloc_entries.extend(reloc::parse_type_b_entries(&type_b_data)?);
        }
    }

    // 5. Parse NIDs and import stubs
    let nid_map = nid::load_nid_database(nid_db)
        .with_context(|| format!("Cannot load NID DB from {}", nid_db.display()))?;
    let import_stubs = if is_prx {
        match prx::parse_module_info(&raw_data, &elf_obj) {
            Ok((stub_top, stub_btm, _)) => {
                imports::parse_import_stubs(
                    &raw_data,
                    &elf_obj,
                    stub_top,
                    stub_btm,
                    &nid_map,
                )
                .unwrap_or_default()
            }
            Err(e) => {
                tracing::warn!(
                    "SceModuleInfo not found: {e}; continuing without import stubs"
                );
                vec![]
            }
        }
    } else {
        vec![]
    };
    tracing::info!("Resolved {} import stubs", import_stubs.len());

    // 6. Run Ghidra headless analysis (reuse existing output if available)
    let ghidra_raw_path = output.with_extension("ghidra_raw.json");
    if ghidra_raw_path.exists() {
        tracing::info!(
            "Using existing {}",
            ghidra_raw_path.display()
        );
    } else if let Some(ghidra) = ghidra_dir {
        run_ghidra_analysis(binary, ghidra, &ghidra_raw_path)?;
    } else {
        tracing::warn!(
            "--ghidra-dir not provided; skipping Ghidra analysis"
        );
        std::fs::write(
            &ghidra_raw_path,
            r#"{"functions":[],"xrefs":[],"constructors":[]}"#,
        )?;
    }

    // 7. Load Ghidra raw output
    let ghidra_json_str = std::fs::read_to_string(&ghidra_raw_path)
        .with_context(|| format!("Cannot read {}", ghidra_raw_path.display()))?;
    let ghidra_data: serde_json::Value =
        serde_json::from_str(&ghidra_json_str).context("Ghidra raw JSON is invalid")?;

    // 8. Merge all data into AnalysisJson
    use psp_parser::analysis_json::*;

    let mut functions: Vec<JsonFunction> =
        serde_json::from_value(ghidra_data["functions"].clone())
            .unwrap_or_default();

    let xrefs: Vec<JsonXref> =
        serde_json::from_value(ghidra_data["xrefs"].clone())
            .unwrap_or_default();

    let constructors: Vec<String> =
        serde_json::from_value(ghidra_data["constructors"].clone())
            .unwrap_or_default();

    // 8.5. HLE entry point discovery
    // Scan decoded instructions near import stub call sites for function
    // pointers passed to sceKernelCreateThread, sceKernelCreateCallback, etc.
    let functions_before = functions.len();
    let hle_result = run_hle_entry_scan(
        &raw_data,
        &elf_obj,
        is_prx,
        &nid_map,
        &import_stubs,
        &segments,
        &seg_data_vecs,
        &mut functions,
        output,
    )?;
    if functions.len() > functions_before {
        tracing::info!(
            "HLE scan: {} -> {} functions (+{})",
            functions_before,
            functions.len(),
            functions.len() - functions_before
        );
    }

    // 8.6. Correct giant function sizes by clamping to next boundary
    let size_corrections =
        crate::hle_entry_scanner::correct_function_sizes(&mut functions);
    if !size_corrections.is_empty() {
        tracing::info!(
            "Corrected {} giant function sizes",
            size_corrections.len()
        );
    }

    // 8.7. Write discovered_entries.json audit file
    let discovered_path = output.with_extension("").with_file_name(
        "discovered_entries.json",
    );
    crate::hle_entry_scanner::write_discovered_entries_json(
        &hle_result,
        &size_corrections,
        &discovered_path,
    )?;

    // 9. Build segment records (base64-encoded, p_memsz bytes).
    let json_segments: Vec<JsonSegment> = segments
        .iter()
        .zip(seg_data_vecs.iter())
        .map(|(s, d)| JsonSegment {
            p_vaddr: format!("0x{:08X}", s.p_vaddr),
            p_filesz: s.p_filesz as u64,
            p_memsz: s.p_memsz as u64,
            p_flags: s.p_flags,
            data_b64: B64.encode(d),
        })
        .collect();

    // 10. Mid-entry detection via Ghidra xrefs (V1-compatible, ANALYSIS-07).
    // Now runs on the corrected function list (HLE entries + size fixes),
    // which means xrefs that previously pointed "inside" a giant function may
    // now correctly target the newly-added standalone functions instead.
    let mid_entries = detect_mid_entries_from_xrefs(&functions, &xrefs);
    tracing::info!(
        "Detected {} mid-function entry points (xref-based)",
        mid_entries.len()
    );

    let module_name = binary
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("unknown")
        .to_lowercase();

    let analysis = AnalysisJson {
        binary_path: binary.display().to_string(),
        module_name,
        heap_base: format!("0x{heap_base:08X}"),
        functions,
        imports: import_stubs
            .iter()
            .map(|s| JsonImport {
                nid: format!("0x{:08X}", s.nid),
                stub_addr: format!("0x{:08X}", s.stub_addr),
                name: s.name.clone(),
                module_name: s.module_name.clone(),
            })
            .collect(),
        relocations: all_reloc_entries
            .iter()
            .map(|r| JsonReloc {
                offset: format!("0x{:08X}", r.offset),
                r_type: r.r_type,
                ofs_base: r.ofs_base,
                addr_base: r.addr_base,
            })
            .collect(),
        xrefs,
        constructors,
        mid_entries,
        segments: json_segments,
    };

    // 11. Write output
    let json_str =
        serde_json::to_string_pretty(&analysis).context("Failed to serialize analysis.json")?;
    std::fs::write(output, json_str)
        .with_context(|| format!("Failed to write {}", output.display()))?;

    tracing::info!(
        "Analysis complete: {} functions, {} imports, {} mid-entries -> {}",
        analysis.functions.len(),
        analysis.imports.len(),
        analysis.mid_entries.len(),
        output.display()
    );
    Ok(())
}
