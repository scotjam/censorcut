//! `censorcut-sync` — sidecar binary for the CensorCut editor.
//!
//! M7.5.1 wires up only the local validation + capping logic; networking
//! lands in M7.5.2 (iroh-gossip). The CLI surface is the same shape that
//! the C++ side will eventually drive, so the two sides can be developed
//! in parallel.
//!
//! Usage:
//!     censorcut-sync \
//!         --feedback ~/.censorcut/feedback.jsonl \
//!         --peers   ~/.censorcut/peers.jsonl     \
//!         --max-peers-bytes 50000000             \
//!         --max-peer-rows-per-day 200            \
//!         validate-stdin    # current subcommand: read JSONL from stdin
//!                           # and append accepted rows to peers.jsonl

use std::io::{self, BufRead};
use std::path::PathBuf;

use anyhow::{Context, Result};
use clap::{Parser, Subcommand};

mod crypto;
mod endorse;
mod network;
mod schema;
mod sink;
mod wire;

use schema::SchemaConfig;

#[derive(Parser, Debug)]
#[command(version, about = "CensorCut peer sync sidecar (M7.5.1)")]
struct Cli {
    /// Local feedback file written by the C++ FeedbackStore.
    #[arg(long, default_value_os_t = default_feedback_path())]
    feedback: PathBuf,

    /// Inbound accepted-category file populated by this sidecar.
    #[arg(long, default_value_os_t = default_peers_path())]
    peers: PathBuf,

    /// Inbound file for rows whose category isn't yet on the accepted
    /// list. The editor reads this to surface category proposals to the
    /// user; on acceptance the rows are promoted to the peers file.
    #[arg(long, default_value_os_t = default_proposed_path())]
    proposed: PathBuf,

    /// Hard cap on the size of the peers file. Default 50 MB.
    #[arg(long, default_value_t = 50 * 1024 * 1024)]
    max_peers_bytes: u64,

    /// Per-peer rows accepted per UTC day.
    #[arg(long, default_value_t = 200)]
    max_peer_rows_per_day: u32,

    /// Hard cap on the size of the proposed file. Default 5 MB.
    #[arg(long, default_value_t = 5 * 1024 * 1024)]
    max_proposed_bytes: u64,

    /// Hard ceiling on the number of distinct proposed category names
    /// we'll track. Stops a flooder from cluttering the UI with fake
    /// category names.
    #[arg(long, default_value_t = 100)]
    max_proposed_categories: u32,

    /// Per-category cap on sample rows kept under proposal.
    #[arg(long, default_value_t = 50)]
    max_proposed_samples_per_category: u32,

    /// Newline-separated file of category names the user has explicitly
    /// approved (in addition to the 12 built-ins). The editor writes
    /// this whenever the user accepts a category from the proposals
    /// surface. Each line is trimmed; lines starting with '#' and blank
    /// lines are ignored. Names are themselves shape-validated before
    /// being trusted (alphanumeric + space/slash/hyphen/period only,
    /// max 64 chars).
    #[arg(long)]
    accepted_categories_file: Option<PathBuf>,

    /// Path to the persistent ed25519 identity (32-byte secret seed).
    /// Created on first run; deleting it gives this installation a
    /// fresh peer key on next launch.
    #[arg(long, default_value_os_t = default_identity_path())]
    identity: PathBuf,

    /// Outbound endorsement entries written by the C++ TrustLedger.
    /// One JSON object per line: {"target":"<pubkey-hex>","score":<f>}.
    /// Polled by the gossip transport; changes trigger a daily-bundled
    /// broadcast.
    #[arg(long, default_value_os_t = default_outbound_endorsements_path())]
    outbound_endorsements: PathBuf,

    /// Inbound endorsements file written by this sidecar (StoredEndorsement
    /// rows, deduplicated per-peer-per-day). The C++ TrustLedger reads
    /// this on launch to populate the bootstrap graph.
    #[arg(long, default_value_os_t = default_endorsements_in_path())]
    endorsements_in: PathBuf,

    #[command(subcommand)]
    command: Cmd,
}

#[derive(Subcommand, Debug)]
enum Cmd {
    /// Read JSONL from stdin (one feedback row per line, optional
    /// "PEER_KEY <space> JSON" prefix); validate, dedup, rate-limit,
    /// and append accepted rows to the peers file. Useful for tests
    /// and as a stepping stone for the iroh-gossip sink.
    ValidateStdin,

    /// Read one feedback-row JSON from stdin, sign it with the local
    /// identity, and write the wire envelope to stdout. Generates the
    /// identity file if absent.
    Sign,

    /// Read one wire envelope from stdin, verify the signature, and
    /// run the inner row through the same sink path the gossip
    /// transport will use. Exit code is non-zero on signature or sink
    /// failure.
    VerifyStdin,

    /// Print the local identity's public key (hex). Creates the
    /// identity file if absent.
    Whoami,

    /// Run the iroh-gossip transport: subscribes to the censorcut
    /// feedback topic, broadcasts new rows from the local feedback
    /// file as they appear, and pipes incoming envelopes through the
    /// same verify+sink path.
    Gossip {
        /// Bootstrap peer specs, one per `--bootstrap` (repeatable).
        /// Format: `<NodeId>` or `<NodeId>@<host:port>`. Without any,
        /// we wait passively for inbound peers.
        #[arg(long, value_name = "NODEID[@HOST:PORT]")]
        bootstrap: Vec<String>,
    },
}

fn default_feedback_path() -> PathBuf {
    let home = std::env::var_os("HOME")
        .or_else(|| std::env::var_os("USERPROFILE"))
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."));
    home.join(".censorcut").join("feedback.jsonl")
}

fn default_peers_path() -> PathBuf {
    let home = std::env::var_os("HOME")
        .or_else(|| std::env::var_os("USERPROFILE"))
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."));
    home.join(".censorcut").join("peers.jsonl")
}

fn default_proposed_path() -> PathBuf {
    let home = std::env::var_os("HOME")
        .or_else(|| std::env::var_os("USERPROFILE"))
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."));
    home.join(".censorcut").join("proposed.jsonl")
}

fn default_identity_path() -> PathBuf {
    let home = std::env::var_os("HOME")
        .or_else(|| std::env::var_os("USERPROFILE"))
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."));
    home.join(".censorcut").join("identity.key")
}

fn default_outbound_endorsements_path() -> PathBuf {
    let home = std::env::var_os("HOME")
        .or_else(|| std::env::var_os("USERPROFILE"))
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."));
    home.join(".censorcut").join("outbound_endorsements.jsonl")
}

fn default_endorsements_in_path() -> PathBuf {
    let home = std::env::var_os("HOME")
        .or_else(|| std::env::var_os("USERPROFILE"))
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."));
    home.join(".censorcut").join("endorsements.jsonl")
}

fn load_schema_cfg(extra_file: Option<&PathBuf>) -> Result<SchemaConfig> {
    let mut cfg = SchemaConfig::builtin();
    let Some(path) = extra_file else { return Ok(cfg); };
    if !path.exists() { return Ok(cfg); }
    let text = std::fs::read_to_string(path)
        .with_context(|| format!("reading {}", path.display()))?;
    let mut accepted_extra = 0usize;
    let mut rejected = 0usize;
    for line in text.lines() {
        let name = line.trim();
        if name.is_empty() || name.starts_with('#') { continue; }
        // Validate the *name* with the same rules `validate_row` will
        // apply on the wire — anything illegal here would just get
        // dropped row-by-row anyway, but flagging it once at startup is
        // friendlier than silently doing nothing.
        let ok = name.chars().count() <= schema::MAX_CATEGORY_LEN
            && name.chars().all(|c| c.is_ascii_alphanumeric()
                                   || matches!(c, ' ' | '/' | '-' | '.'));
        if !ok {
            eprintln!("censorcut-sync: ignoring invalid accepted-category {:?}", name);
            rejected += 1;
            continue;
        }
        if !cfg.accepted_categories.iter().any(|c| c == name) {
            cfg.accepted_categories.push(name.to_string());
            accepted_extra += 1;
        }
    }
    eprintln!(
        "censorcut-sync: schema cfg — {} built-ins + {} user-accepted (rejected {} malformed)",
        schema::BUILTIN_CATEGORIES.len(), accepted_extra, rejected,
    );
    Ok(cfg)
}

#[tokio::main]
async fn main() -> Result<()> {
    let cli = Cli::parse();
    let limits = sink::SinkLimits {
        max_total_bytes:                   cli.max_peers_bytes,
        max_rows_per_peer_per_day:         cli.max_peer_rows_per_day,
        max_proposed_bytes:                cli.max_proposed_bytes,
        max_proposed_categories:           cli.max_proposed_categories,
        max_proposed_samples_per_category: cli.max_proposed_samples_per_category,
    };
    let schema_cfg = load_schema_cfg(cli.accepted_categories_file.as_ref())?;

    match cli.command {
        Cmd::ValidateStdin => {
            let mut sink = sink::PeerSink::open_with_proposed(
                &cli.peers, Some(&cli.proposed), limits, schema_cfg)?;
            let stdin = io::stdin();
            for line in stdin.lock().lines() {
                let line = line?;
                let line = line.trim();
                if line.is_empty() { continue; }
                let (peer_key, raw_json) = match line.find(' ') {
                    Some(idx) => (&line[..idx], &line[idx + 1..]),
                    None      => ("self", line),
                };
                match sink.try_accept(raw_json, peer_key) {
                    Ok(stored)  => eprintln!("accepted fp={} peer={}",
                                             &stored.fingerprint[..12], stored.peer_key),
                    Err(e)      => eprintln!("rejected: {}", e),
                }
            }
            eprintln!("stats: {:?}", sink.stats);
        }

        Cmd::Sign => {
            let id = crypto::Identity::load_or_create(&cli.identity)?;
            let mut row = String::new();
            io::stdin().lock().read_line(&mut row)?;
            let row = row.trim_end_matches(&['\r','\n'][..]);
            if row.is_empty() {
                anyhow::bail!("expected a feedback-row JSON on stdin");
            }
            let env = wire::seal(row, &id);
            println!("{}", wire::to_line(&env)?);
        }

        Cmd::VerifyStdin => {
            let mut sink = sink::PeerSink::open_with_proposed(
                &cli.peers, Some(&cli.proposed), limits, schema_cfg)?;
            let stdin = io::stdin();
            let mut accepted = 0u64;
            let mut rejected = 0u64;
            for line in stdin.lock().lines() {
                let line = line?;
                let line = line.trim();
                if line.is_empty() { continue; }
                match wire::open(line) {
                    Ok((row_json, peer_key)) => {
                        match sink.try_accept(&row_json, &peer_key) {
                            Ok(stored) => {
                                accepted += 1;
                                eprintln!("accepted fp={} peer={}…",
                                          &stored.fingerprint[..12],
                                          &stored.peer_key[..16.min(stored.peer_key.len())]);
                            }
                            Err(e) => {
                                rejected += 1;
                                eprintln!("sink rejected: {}", e);
                            }
                        }
                    }
                    Err(e) => {
                        rejected += 1;
                        eprintln!("envelope rejected: {}", e);
                    }
                }
            }
            eprintln!("verify-stdin: accepted={} rejected={} stats={:?}",
                      accepted, rejected, sink.stats);
        }

        Cmd::Whoami => {
            let id = crypto::Identity::load_or_create(&cli.identity)?;
            println!("{}", id.public_hex());
        }

        Cmd::Gossip { bootstrap } => {
            let opts = network::GossipOptions {
                identity_path:              cli.identity.clone(),
                feedback_path:              cli.feedback.clone(),
                peers_path:                 cli.peers.clone(),
                proposed_path:              cli.proposed.clone(),
                outbound_endorsements_path: cli.outbound_endorsements.clone(),
                endorsements_in_path:       cli.endorsements_in.clone(),
                bootstrap,
                limits,
                schema_cfg,
            };
            network::run(opts).await?;
        }
    }
    Ok(())
}
