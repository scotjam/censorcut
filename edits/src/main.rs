//! `censorcut-edits` — local-first repository server for edit packs.
//!
//! M8.5.1 ships only the binary skeleton: schema validation
//! (sync/src/schema.rs has a peer-rows analogue, this is its edit-pack
//! sibling), filesystem storage layout, allowlist parsing, and a
//! `check` subcommand that loads the data dir and reports a sane
//! summary. The HTTP routes (GET/POST /v1/edits) land in M8.5.2.

use std::collections::HashSet;
use std::path::PathBuf;

use anyhow::{Context, Result};
use clap::{Parser, Subcommand};

mod config;
mod schema;
mod server;
mod storage;

#[derive(Parser, Debug)]
#[command(version, about = "CensorCut local-first edits repository (M8.5.1)")]
struct Cli {
    /// Filesystem root for stored packs. Default ~/.censorcut/edits.
    #[arg(long, default_value_os_t = default_data_dir())]
    data_dir: PathBuf,

    /// Newline-separated file of allowlisted publisher pubkeys (hex).
    /// Empty / missing file = no allowlist (every signature-valid pack
    /// is accepted; combine with --bind 127.0.0.1 to keep that local).
    #[arg(long)]
    allow_pubkeys: Option<PathBuf>,

    /// HTTP bind address. Default 127.0.0.1:8765 — local-only. To
    /// expose the server to a LAN/internet host, set --bind 0.0.0.0:PORT
    /// explicitly. Used in M8.5.2 once the routes land.
    #[arg(long, default_value = "127.0.0.1:8765")]
    bind: String,

    #[command(subcommand)]
    command: Cmd,
}

#[derive(Subcommand, Debug)]
enum Cmd {
    /// Walk the data dir, validate every pack, and print a summary.
    /// Useful for catching corruption / unsigned packs before serving.
    Check,
    /// Print the configured paths and exit 0. Sanity-check launcher
    /// scripts use this to confirm the binary will use the dirs they
    /// expect.
    Paths,
    /// Run the HTTP server bound to --bind. By default the bind is
    /// 127.0.0.1:8765 — local-only; the user opts into LAN/WAN
    /// access by setting --bind 0.0.0.0:PORT explicitly.
    Serve,
}

fn default_data_dir() -> PathBuf {
    let home = std::env::var_os("HOME")
        .or_else(|| std::env::var_os("USERPROFILE"))
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."));
    home.join(".censorcut").join("edits")
}

fn load_allowlist(p: Option<&PathBuf>) -> Result<HashSet<String>> {
    let Some(path) = p else { return Ok(HashSet::new()); };
    let (set, skipped) = config::load_allowlist(path)
        .with_context(|| format!("loading allowlist {}", path.display()))?;
    if skipped > 0 {
        eprintln!(
            "censorcut-edits: warning — {skipped} malformed line(s) in allowlist; ignored"
        );
    }
    Ok(set)
}

#[tokio::main]
async fn main() -> Result<()> {
    let cli = Cli::parse();

    match cli.command {
        Cmd::Paths => {
            println!("data_dir       : {}", cli.data_dir.display());
            println!("allow_pubkeys  : {}",
                     cli.allow_pubkeys.as_deref()
                         .map(|p| p.display().to_string())
                         .unwrap_or_else(|| "<none — open repo>".to_string()));
            println!("bind           : {}", cli.bind);
        }
        Cmd::Check => {
            let allow = load_allowlist(cli.allow_pubkeys.as_ref())?;
            let repo  = storage::Repo::open(&cli.data_dir)?;

            let mut total = 0usize;
            let mut by_film: std::collections::BTreeMap<String, usize> =
                Default::default();
            let mut sig_failures = 0usize;
            let mut not_allowed  = 0usize;
            let mut malformed    = 0usize;

            for path in repo.walk() {
                total += 1;
                let raw = match std::fs::read_to_string(&path) {
                    Ok(s)  => s,
                    Err(_) => { malformed += 1; continue; }
                };
                match schema::validate_shape(&raw) {
                    Ok(p) => {
                        if !schema::is_author_allowed(&p, &allow) {
                            not_allowed += 1;
                        }
                        if schema::verify_signature(&p).is_err() {
                            sig_failures += 1;
                        }
                        *by_film.entry(p.film_fp.clone()).or_insert(0) += 1;
                    }
                    Err(e) => {
                        malformed += 1;
                        eprintln!("malformed: {} ({})", path.display(), e);
                    }
                }
            }
            println!("data_dir          : {}", cli.data_dir.display());
            println!("packs found       : {total}");
            println!("distinct films    : {}", by_film.len());
            println!("malformed         : {malformed}");
            println!("signature failures: {sig_failures}");
            println!("not in allowlist  : {not_allowed}");
            println!("allowlist size    : {}",
                     if allow.is_empty() { "0 (open)".to_string() }
                                         else { allow.len().to_string() });
        }
        Cmd::Serve => {
            let allow = load_allowlist(cli.allow_pubkeys.as_ref())?;
            let repo  = storage::Repo::open(&cli.data_dir)?;
            let state = server::AppState {
                repo:  std::sync::Arc::new(repo),
                allow: std::sync::Arc::new(allow),
            };
            let app  = server::router(state);

            // Refuse to silently expose the server to a non-loopback
            // bind unless the user typed it themselves. Print a banner
            // when they have so it's obvious what's happening.
            let bind: std::net::SocketAddr = cli.bind.parse()
                .with_context(|| format!("could not parse --bind {:?}", cli.bind))?;
            if !bind.ip().is_loopback() {
                eprintln!("censorcut-edits: NOTE — bound to {bind}, NOT loopback. \
                          Anyone who can reach this address can query which film \
                          fingerprints you host. Make sure that's what you want.");
            }

            let listener = tokio::net::TcpListener::bind(bind).await
                .with_context(|| format!("bind {bind}"))?;
            eprintln!("censorcut-edits: serving on http://{} (data: {})",
                      bind, cli.data_dir.display());

            axum::serve(listener, app)
                .with_graceful_shutdown(async {
                    let _ = tokio::signal::ctrl_c().await;
                    eprintln!("censorcut-edits: shutting down");
                })
                .await?;
        }
    }
    Ok(())
}
