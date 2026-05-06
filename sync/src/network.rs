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
    pub identity_path:   std::path::PathBuf,
    pub feedback_path:   std::path::PathBuf,
    pub peers_path:      std::path::PathBuf,
    pub proposed_path:   std::path::PathBuf,
    pub bootstrap:       Vec<String>,
    pub limits:          crate::sink::SinkLimits,
    pub schema_cfg:      crate::schema::SchemaConfig,
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
    let (mut sender, mut receiver) = topic_sub.split();

    let sink = Arc::new(Mutex::new(PeerSink::open_with_proposed(
        &opts.peers_path, Some(&opts.proposed_path),
        opts.limits, opts.schema_cfg)?));

    // Receive task ------------------------------------------------------
    let recv_sink = sink.clone();
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
                        Ok((row_json, peer_key)) => {
                            let mut s = recv_sink.lock().await;
                            match s.try_accept(&row_json, &peer_key) {
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
                if let Err(e) = sender.broadcast(serialized.into_bytes().into()).await {
                    eprintln!("gossip: broadcast error: {e}");
                }
            }
        }
    });

    // Wait for ctrl-c.
    tokio::signal::ctrl_c().await.ok();
    eprintln!("censorcut-sync: shutting down");
    recv_handle.abort();
    bc_handle.abort();
    Ok(())
}

// Suppress unused-Path warning when the network code is feature-gated.
#[allow(dead_code)]
fn _path_kept(_p: &Path) {}

// Suppress unused-bail warning if no error path uses it.
#[allow(dead_code)]
fn _bail_kept() -> Result<()> { bail!("unused") }
