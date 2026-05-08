//! Iroh-gossip transport for the feedback swarm.
//!
//! Topic ID = sha256("censorcut/feedback/v1") so every client participates
//! in the same swarm without a central directory. Each broadcast is the
//! `wire::Envelope` JSON line — peers verify the signature, hand the row
//! to the same `PeerSink` the local CLI tools use, which means **every
//! row that lands on disk has gone through the full M7.5.1 + M7.5.2a
//! validation stack regardless of where it came from**.

use std::path::Path;
use std::sync::Arc;
use std::time::Duration;

use anyhow::{bail, Context, Result};
use chrono::{Datelike, Utc};
use sha2::{Digest, Sha256};
use tokio::sync::Mutex;

use futures::TryStreamExt;

use iroh::{Endpoint, NodeAddr, NodeId};
use iroh_gossip::{
    api::Event,
    net::Gossip,
    proto::TopicId,
};

use crate::crypto::Identity;
use crate::endorse::{self, EndorsementBatch, EndorsementEntry, EndorsementSink};
use crate::sink::PeerSink;
use crate::wire;

const TOPIC_LABEL: &str = "censorcut/feedback/v1";

/// Derive a deterministic 32-byte topic id from the textual label so
/// every client lands on the same gossip topic without coordinating.
fn topic_id() -> TopicId {
    let mut h = Sha256::new();
    h.update(TOPIC_LABEL.as_bytes());
    let bytes: [u8; 32] = h.finalize().into();
    TopicId::from_bytes(bytes)
}

fn today_yyyymmdd() -> u32 {
    let now = Utc::now();
    (now.year() as u32) * 10000 + now.month() * 100 + now.day()
}

/// Parse a `<NodeId>` or `<NodeId>@<host:port>` bootstrap entry. Iroh's
/// own discovery handles addresses if we only know the NodeId.
fn parse_bootstrap(spec: &str) -> Result<NodeAddr> {
    let (id_str, addrs) = match spec.split_once('@') {
        Some((id, addr)) => (id, vec![addr.parse()?]),
        None             => (spec, vec![]),
    };
    let id: NodeId = id_str.parse().context("bootstrap NodeId is not valid")?;
    let mut na = NodeAddr::new(id);
    for a in addrs { na = na.with_direct_addresses([a]); }
    Ok(na)
}

pub struct GossipOptions {
    pub identity_path:                std::path::PathBuf,
    pub feedback_path:                std::path::PathBuf,
    pub peers_path:                   std::path::PathBuf,
    pub proposed_path:                std::path::PathBuf,
    /// Outbound endorsement entries written by the C++ TrustLedger.
    /// Each line is `{"target":"<pubkey-hex>","score":<float>}`. The
    /// broadcaster bundles them into a daily EndorsementBatch.
    pub outbound_endorsements_path:   std::path::PathBuf,
    /// Inbound endorsements file written by this sidecar; the C++
    /// TrustLedger reads it on launch to populate the bootstrap graph.
    pub endorsements_in_path:         std::path::PathBuf,
    pub bootstrap:                    Vec<String>,
    pub limits:                       crate::sink::SinkLimits,
    pub schema_cfg:                   crate::schema::SchemaConfig,
}

/// Run the gossip transport until the shutdown signal fires. Spawns:
///   - a broadcaster task that tails `feedback_path` for new lines and
///     sends them through the topic.
///   - a receiver task that pulls envelopes from the topic, verifies
///     them, and hands the inner rows to a shared PeerSink.
pub async fn run(opts: GossipOptions) -> Result<()> {
    let id = Identity::load_or_create(&opts.identity_path)?;
    eprintln!("censorcut-sync: identity {}", id.public_hex());

    let endpoint = Endpoint::builder()
        .alpns(vec![iroh_gossip::ALPN.to_vec()])
        .discovery_n0()
        .bind()
        .await?;
    eprintln!("censorcut-sync: bound iroh node {}",
              endpoint.node_id());

    let gossip = Gossip::builder().spawn(endpoint.clone());

    let mut bootstrap_nodes = Vec::new();
    for spec in &opts.bootstrap {
        match parse_bootstrap(spec) {
            Ok(na) => {
                endpoint.add_node_addr(na.clone()).ok();
                bootstrap_nodes.push(na.node_id);
            }
            Err(e) => eprintln!("censorcut-sync: skipping bad bootstrap {:?}: {}", spec, e),
        }
    }
    if bootstrap_nodes.is_empty() {
        eprintln!("censorcut-sync: no bootstrap nodes specified — \
                  waiting passively for inbound peers");
    }

    let topic = topic_id();
    let topic_sub = gossip.subscribe(topic, bootstrap_nodes)
        .await
        .context("subscribe to gossip topic")?;
    let (sender, mut receiver) = topic_sub.split();
    // Wrap the sender in an Arc<Mutex<>> so the feedback and
    // endorsement broadcaster tasks can share it without contending
    // ownership. Lock duration is microseconds — neither task starves
    // the other.
    let sender = Arc::new(Mutex::new(sender));

    let sink = Arc::new(Mutex::new(PeerSink::open_with_proposed(
        &opts.peers_path, Some(&opts.proposed_path),
        opts.limits, opts.schema_cfg)?));

    let endorse_sink = Arc::new(Mutex::new(
        EndorsementSink::open(&opts.endorsements_in_path)
            .context("opening endorsements inbound sink")?));

    // Receive task ------------------------------------------------------
    let recv_sink = sink.clone();
    let recv_endorse = endorse_sink.clone();
    let recv_handle = tokio::spawn(async move {
        loop {
            let evt = match receiver.try_next().await {
                Ok(Some(evt)) => evt,
                Ok(None) => break,
                Err(e) => { eprintln!("censorcut-sync: gossip recv error: {e}"); break; }
            };
            match evt {
                Event::Received(msg) => {
                    let line = match std::str::from_utf8(&msg.content) {
                        Ok(s)  => s,
                        Err(_) => continue,
                    };
                    match wire::open(line) {
                        Ok((inner_json, peer_key)) => {
                            // Discriminator: endorsement batches carry
                            // `"kind":"endorsements"`; feedback rows do
                            // not. The substring check is cheap and
                            // unambiguous because feedback rows have
                            // no `kind` field per the schema.
                            if inner_json.contains(r#""kind":"endorsements""#) {
                                match endorse::validate_endorsement_shape(&inner_json) {
                                    Ok(batch) => {
                                        let mut e = recv_endorse.lock().await;
                                        if let Err(err) = e.put(&peer_key, batch) {
                                            eprintln!("gossip: endorsement sink rejected: {err}");
                                        } else if let Err(err) = e.flush() {
                                            eprintln!("gossip: endorsement flush failed: {err}");
                                        } else {
                                            eprintln!(
                                                "gossip: accepted endorsement batch from peer {}…",
                                                &peer_key[..16.min(peer_key.len())]);
                                        }
                                    }
                                    Err(err) => eprintln!(
                                        "gossip: endorsement rejected: {err}"),
                                }
                                continue;
                            }
                            let mut s = recv_sink.lock().await;
                            match s.try_accept(&inner_json, &peer_key) {
                                Ok(stored) => eprintln!(
                                    "gossip: accepted fp={} from peer {}…",
                                    &stored.fingerprint[..12],
                                    &stored.peer_key[..16.min(stored.peer_key.len())]),
                                Err(e) => eprintln!("gossip: sink rejected: {e}"),
                            }
                        }
                        Err(e) => eprintln!("gossip: envelope rejected: {e}"),
                    }
                }
                Event::NeighborUp(node) => {
                    eprintln!("gossip: neighbor up {node}");
                }
                Event::NeighborDown(node) => {
                    eprintln!("gossip: neighbor down {node}");
                }
                Event::Lagged => {
                    eprintln!("gossip: receiver lagged — some messages dropped");
                }
            }
        }
    });

    // Broadcast task ----------------------------------------------------
    // For now: poll feedback.jsonl every 2 s and broadcast any newly-
    // appended lines. We can swap to a notify watcher in a follow-up
    // — polling is simpler and a 2 s lag on outbound is unobjectionable
    // when downstream peers care about decisions over hours-to-days, not
    // milliseconds.
    let bc_id = id.clone();
    let bc_path = opts.feedback_path.clone();
    let bc_sender = sender.clone();
    let bc_handle = tokio::spawn(async move {
        let mut last_len: u64 = 0;
        loop {
            tokio::time::sleep(Duration::from_secs(2)).await;
            let bytes = match tokio::fs::read(&bc_path).await {
                Ok(b)  => b,
                Err(_) => continue,
            };
            if (bytes.len() as u64) <= last_len {
                last_len = bytes.len() as u64;
                continue;
            }
            let new = &bytes[last_len as usize..];
            last_len = bytes.len() as u64;
            let text = match std::str::from_utf8(new) {
                Ok(s)  => s,
                Err(_) => continue,
            };
            for line in text.lines() {
                let line = line.trim();
                if line.is_empty() { continue; }
                let env = wire::seal(line, &bc_id);
                let serialized = match wire::to_line(&env) {
                    Ok(s)  => s,
                    Err(_) => continue,
                };
                let mut s = bc_sender.lock().await;
                if let Err(e) = s.broadcast(serialized.into_bytes().into()).await {
                    eprintln!("gossip: broadcast error: {e}");
                }
            }
        }
    });

    // Endorsement broadcaster --------------------------------------------
    // Polls outbound_endorsements.jsonl (one {target, score} JSON per line,
    // written by the C++ TrustLedger). On startup or whenever the file's
    // content hash changes, bundles entries into a daily EndorsementBatch,
    // signs, and broadcasts.
    let eb_id      = id.clone();
    let eb_path    = opts.outbound_endorsements_path.clone();
    let eb_sender  = sender.clone();
    let eb_handle  = tokio::spawn(async move {
        let mut last_hash: u64 = 0;
        loop {
            tokio::time::sleep(Duration::from_secs(60)).await;
            let bytes = match tokio::fs::read(&eb_path).await {
                Ok(b)  => b,
                Err(_) => continue,  // file doesn't exist yet
            };
            // Rolling Fletcher-style hash to detect content change without
            // pulling in a hashing dep that's not already available
            // through serde / sha2.
            let mut h: u64 = 0xcbf29ce484222325;
            for &b in &bytes {
                h ^= b as u64;
                h = h.wrapping_mul(0x100000001b3);
            }
            if h == last_hash { continue; }
            last_hash = h;

            // Parse outbound entries.
            let text = match std::str::from_utf8(&bytes) {
                Ok(s)  => s,
                Err(_) => continue,
            };
            let mut entries = Vec::<EndorsementEntry>::new();
            for line in text.lines() {
                let line = line.trim();
                if line.is_empty() { continue; }
                let parsed: Result<serde_json::Value, _> = serde_json::from_str(line);
                if let Ok(v) = parsed {
                    let target = v.get("target").and_then(|x| x.as_str()).unwrap_or("");
                    let score  = v.get("score").and_then(|x| x.as_f64()).unwrap_or(0.0);
                    if !target.is_empty() {
                        entries.push(EndorsementEntry {
                            target: target.to_string(),
                            score:  score as f32,
                        });
                    }
                }
                if entries.len() >= endorse::MAX_ENTRIES_PER_BATCH { break; }
            }
            if entries.is_empty() { continue; }

            let day_utc = today_yyyymmdd();
            let batch = EndorsementBatch {
                schema:  1,
                kind:    "endorsements".to_string(),
                day_utc,
                entries,
            };
            let raw = match serde_json::to_string(&batch) {
                Ok(s)  => s,
                Err(_) => continue,
            };
            let env = wire::seal(&raw, &eb_id);
            let serialized = match wire::to_line(&env) {
                Ok(s)  => s,
                Err(_) => continue,
            };
            let mut s = eb_sender.lock().await;
            if let Err(e) = s.broadcast(serialized.into_bytes().into()).await {
                eprintln!("gossip: endorsement broadcast error: {e}");
            } else {
                eprintln!("gossip: broadcast endorsement batch ({} entries, day {})",
                          batch.entries.len(), day_utc);
            }
        }
    });

    // Wait for ctrl-c.
    tokio::signal::ctrl_c().await.ok();
    eprintln!("censorcut-sync: shutting down");
    recv_handle.abort();
    bc_handle.abort();
    eb_handle.abort();
    Ok(())
}

// Suppress unused-Path warning when the network code is feature-gated.
#[allow(dead_code)]
fn _path_kept(_p: &Path) {}

// Suppress unused-bail warning if no error path uses it.
#[allow(dead_code)]
fn _bail_kept() -> Result<()> { bail!("unused") }
