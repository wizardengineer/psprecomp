mod analyze;
mod config;
mod dump;
mod hle_entry_scanner;
mod recompile;
mod report;

use clap::{Parser, Subcommand};

#[derive(Parser)]
#[command(name = "psprecomp", version, about = "PSP static recompiler")]
struct Cli {
    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    /// Analyze a PSP PRX or ELF binary and produce analysis.json
    Analyze {
        /// Path to the PSP binary (PRX or ELF)
        binary: std::path::PathBuf,
        /// Output path for analysis.json (default: analysis.json)
        #[arg(short, long, default_value = "analysis.json")]
        output: std::path::PathBuf,
        /// Path to Ghidra install directory
        #[arg(long, env = "GHIDRA_INSTALL_DIR")]
        ghidra_dir: Option<std::path::PathBuf>,
        /// Path to ppsspp_niddb.xml
        #[arg(long, default_value = "data/niddb/ppsspp_niddb.xml")]
        nid_db: std::path::PathBuf,
    },
    /// Recompile analysis.json to a C++17 project in the output directory
    Recompile {
        /// Path to analysis.json from psprecomp analyze
        analysis: std::path::PathBuf,
        /// Output directory for generated C++ project (default: output/)
        #[arg(short, long, default_value = "output")]
        output: std::path::PathBuf,
        /// Optional TOML game config for stubs, skips, patches
        #[arg(long)]
        config: Option<std::path::PathBuf>,
        /// Functions per .cpp file (default: 50)
        #[arg(long, default_value_t = 50usize)]
        batch_size: usize,
        /// Fail (exit nonzero) unless the final function count equals N
        #[arg(long)]
        expect_functions: Option<usize>,
        /// Fail (exit nonzero) unless the final mid-entry count equals N
        #[arg(long)]
        expect_mid_entries: Option<usize>,
    },
    /// Dump analysis.json fields, or one function's generated C++ (0xADDR)
    Dump {
        /// Path to analysis.json
        analysis: std::path::PathBuf,
        /// A hex function address (e.g. 0x0881E7A8 — emits that function's
        /// C++ exactly as it appears in its batch file; PSPRECOMP_CROSS_MID
        /// applies as in recompile), or a list mode:
        /// functions | imports | relocations | segments | mid_entries
        target: Option<String>,
        /// What to dump: functions | imports | relocations | segments | mid_entries
        #[arg(long, conflicts_with = "target")]
        what: Option<String>,
        /// Optional TOML game config (stubs, skips, patches) — pass the same
        /// config used for recompile so the dumped C++ matches the batch output
        #[arg(long)]
        config: Option<std::path::PathBuf>,
    },
}

fn main() -> anyhow::Result<()> {
    let cli = Cli::parse();
    match cli.command {
        Commands::Analyze { binary, output, ghidra_dir, nid_db } => {
            tracing_subscriber::fmt()
                .with_env_filter(
                    tracing_subscriber::EnvFilter::from_default_env()
                        .add_directive(tracing::Level::INFO.into()),
                )
                .init();
            crate::analyze::run_analyze(&binary, &output, ghidra_dir.as_ref(), &nid_db)?;
        }
        Commands::Recompile {
            analysis, output, config, batch_size, expect_functions, expect_mid_entries,
        } => {
            tracing_subscriber::fmt()
                .with_env_filter(
                    tracing_subscriber::EnvFilter::from_default_env()
                        .add_directive(tracing::Level::INFO.into()),
                )
                .init();
            let opts = crate::recompile::RecompileOptions {
                config_path: config,
                batch_size,
                expect_functions,
                expect_mid_entries,
            };
            crate::recompile::run_recompile(&analysis, &output, &opts)?;
        }
        Commands::Dump { analysis, target, what, config } => {
            // stderr writer: the single-function mode prints ONLY the C++ on
            // stdout so it pipes cleanly; all tracing diagnostics go to stderr.
            tracing_subscriber::fmt()
                .with_writer(std::io::stderr)
                .with_env_filter(
                    tracing_subscriber::EnvFilter::from_default_env()
                        .add_directive(tracing::Level::INFO.into()),
                )
                .init();
            const LIST_MODES: &[&str] =
                &["functions", "imports", "relocations", "segments", "mid_entries"];
            match (target.as_deref(), what.as_deref()) {
                (None, Some(w)) => crate::recompile::run_dump(&analysis, w)?,
                (None, None) => crate::recompile::run_dump(&analysis, "functions")?,
                (Some(t), _) if LIST_MODES.contains(&t) => {
                    crate::recompile::run_dump(&analysis, t)?;
                }
                (Some(t), _) => {
                    crate::dump::run_dump_function(&analysis, t, config.as_deref())?;
                }
            }
        }
    }
    Ok(())
}
