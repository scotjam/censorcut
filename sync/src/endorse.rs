//! Endorsement broadcasts (M7.6.c).
//!
//! An *endorsement* is a peer's declaration "I trust this other pubkey
//! at score X". Peers publish a small batch of their highest-trust
//! endorsements (up to 50 entries, scores ≥ 0.8) so receivers can use
//! them as transitive bootstrap input — the local `TrustLedger` walks
//! the published graph with damping and a hard cap to compute a small
//! starting weight for previously-unseen pubkeys.
//!
//! Threat model deltas vs. feedback rows:
//!   - **Spoofing**: catastrophic if unchecked, because tricking one
//!     trusted peer's apparent endorsement set lets an attacker bootstrap
//!     to that peer's neighborhood. Signature verification (handled by
//!     the existing wire envelope) is required, not optional.
//!   - **Flood / spam**: per-peer-per-day cap of 1 batch (latest wins).
//!     A peer can't republish 100 batches/day to inflate apparent activity.
//!   - **Sybil**: each peer publishing endorsements can only nominate up
//!     to 50 targets. A million Sybil peers don't help — the consumer's
//!     bootstrap is capped (kBootstrapCap = 0.4) regardless of how many
//!     paths converge.
//!
//! Wire shape (carried in the existing wire::Envelope's `r` field):
//!
//! ```json
//! {
//!   "schema":   1,
//!   "kind":     "endorsements",
//!   "day_utc":  20250507,
//!   "entries":  [{"target":"<64-hex>", "score": 1.6}, ...]
//! }
//! ```
//!
//! The author isn't repeated inside `r` because the envelope's `k`
//! (public key) already binds the payload to the publisher; duplicating
//! it here would let the two diverge.

use std::collections::HashMap;
use std::fs::OpenOptions;
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

/// Hard cap on entries inside a single batch. Keeps wire payload bounded
/// regardless of how many peers a sender has accumulated.
pub const MAX_ENTRIES_PER_BATCH: usize = 50;

/// Hard cap on the wire payload bytes for one endorsement batch (before
/// envelope overhead). 50 entries × ~80 bytes each ≈ 4 KB; round up to
/// give headroom for valid JSON whitespace.
pub const MAX_BATCH_BYTES: usize = 8 * 1024;

/// Hard cap on the on-disk endorsements.jsonl file. Same reasoning as
/// the peers.jsonl cap: prevents a flood from filling the user's disk.
pub const MAX_TOTAL_BYTES: u64 = 4 * 1024 * 1024;

/// Trust scores are bounded to TrustLedger's [0, 2.0] range. A peer
/// publishing scores outside that band is malformed, not malicious — drop.
pub const MIN_SCORE: f32 = 0.0;
pub const MAX_SCORE: f32 = 2.0;

/// Pubkey hex must be exactly 64 lowercase hex chars (32-byte ed25519).
pub const PUBKEY_HEX_LEN: usize = 64;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct EndorsementEntry {
    /// Hex of the target's ed25519 public key. 64 chars, lowercase.
    pub target: String,
    /// The publisher's trust score for that target. [0, 2.0].
    pub score:  f32,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct EndorsementBatch {
    pub schema:   u32,
    /// Discriminator; "endorsements". Lets us tell endorsement envelopes
    /// from feedback envelopes when both share the wire envelope shape.
    pub kind:     String,
    /// YYYYMMDD UTC. Used as the rate-limit bucket — a peer can publish
    /// at most one batch per day-of-year.
    pub day_utc:  u32,
    pub entries:  Vec<EndorsementEntry>,
}

#[derive(Debug, thiserror::Error)]
pub enum EndorsementValidateError {
    #[error("payload too large: {0} bytes (max {})", MAX_BATCH_BYTES)]
    TooLarge(usize),
    #[error("malformed JSON: {0}")]
    BadJson(serde_json::Error),
    #[error("schema version {0}; only 1 supported")]
    SchemaVersion(u32),
    #[error("kind {0:?} not supported here; expected \"endorsements\"")]
    BadKind(String),
    #[error("day_utc {0} out of plausible range")]
    BadDayUtc(u32),
    #[error("entries count {0} > {} max", MAX_ENTRIES_PER_BATCH)]
    TooManyEntries(usize),
    #[error("entry #{idx} pubkey {pubkey:?} is not 64 lowercase hex chars")]
    BadPubkey { idx: usize, pubkey: String },
    #[error("entry #{idx} score {score} out of [0, 2.0]")]
    BadScore { idx: usize, score: f32 },
    #[error("duplicate target {pubkey:?} within one batch")]
    DuplicateTarget { pubkey: String },
}

fn is_lower_hex(s: &str) -> bool {
    s.len() == PUBKEY_HEX_LEN
        && s.chars().all(|c| matches!(c, '0'..='9' | 'a'..='f'))
}

/// Validate the wire payload (the `r` field of the wire envelope) for an
/// endorsement broadcast. The author key is *not* checked here — that's
/// done by the envelope's signature path. We do enforce shape only.
pub fn validate_endorsement_shape(raw: &str)
    -> Result<EndorsementBatch, EndorsementValidateError>
{
    if raw.len() > MAX_BATCH_BYTES {
        return Err(EndorsementValidateError::TooLarge(raw.len()));
    }
    let batch: EndorsementBatch = serde_json::from_str(raw)
        .map_err(EndorsementValidateError::BadJson)?;
    if batch.schema != 1 {
        return Err(EndorsementValidateError::SchemaVersion(batch.schema));
    }
    if batch.kind != "endorsements" {
        return Err(EndorsementValidateError::BadKind(batch.kind));
    }
    // Sanity-bound the day. Anything before 2024 or > year 2200 is bogus.
    if batch.day_utc < 20240101 || batch.day_utc > 22000101 {
        return Err(EndorsementValidateError::BadDayUtc(batch.day_utc));
    }
    if batch.entries.len() > MAX_ENTRIES_PER_BATCH {
        return Err(EndorsementValidateError::TooManyEntries(batch.entries.len()));
    }
    let mut seen: std::collections::HashSet<&str> = std::collections::HashSet::new();
    for (idx, e) in batch.entries.iter().enumerate() {
        if !is_lower_hex(&e.target) {
            return Err(EndorsementValidateError::BadPubkey {
                idx,
                pubkey: e.target.clone(),
            });
        }
        if !(MIN_SCORE..=MAX_SCORE).contains(&e.score) || !e.score.is_finite() {
            return Err(EndorsementValidateError::BadScore {
                idx,
                score: e.score,
            });
        }
        if !seen.insert(e.target.as_str()) {
            return Err(EndorsementValidateError::DuplicateTarget {
                pubkey: e.target.clone(),
            });
        }
    }
    Ok(batch)
}

/// One stored endorsement row on disk: the author's pubkey, the day,
/// and the batch payload. We dedup by `(peer_key, day_utc)` — only the
/// latest batch per peer per day is kept, which prevents flood spam.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct StoredEndorsement {
    pub peer_key: String,
    #[serde(flatten)]
    pub batch:    EndorsementBatch,
}

#[derive(Debug, thiserror::Error)]
pub enum EndorsementSinkError {
    #[error("io: {0}")]
    Io(#[from] std::io::Error),
    #[error("validate: {0}")]
    Validate(#[from] EndorsementValidateError),
    #[error("file would exceed cap of {} bytes", MAX_TOTAL_BYTES)]
    OverCap,
}

/// File-backed dedup index over endorsements.jsonl.
///
/// Reads the file on construction into an in-memory map keyed on
/// `(peer_key, day_utc)`. Insertions overwrite older entries from the
/// same peer-day. `flush()` rewrites the whole file — fine because the
/// file is small (≤ 4 MB cap × however many distinct peer-days fit).
pub struct EndorsementSink {
    path:    PathBuf,
    /// (peer_key, day_utc) -> StoredEndorsement
    by_key:  HashMap<(String, u32), StoredEndorsement>,
    bytes:   u64,
}

impl EndorsementSink {
    pub fn open<P: AsRef<Path>>(path: P) -> Result<Self, EndorsementSinkError> {
        let path = path.as_ref().to_path_buf();
        let mut me = EndorsementSink {
            path,
            by_key:  HashMap::new(),
            bytes:   0,
        };
        if me.path.exists() {
            let f = std::fs::File::open(&me.path)?;
            let r = BufReader::new(f);
            for line in r.lines() {
                let line = match line { Ok(l) => l, Err(_) => continue };
                if line.is_empty() { continue; }
                let stored: StoredEndorsement = match serde_json::from_str(&line) {
                    Ok(s) => s, Err(_) => continue,
                };
                me.by_key.insert((stored.peer_key.clone(), stored.batch.day_utc), stored);
            }
            me.recompute_bytes();
        }
        Ok(me)
    }

    fn recompute_bytes(&mut self) {
        let mut total: u64 = 0;
        for s in self.by_key.values() {
            if let Ok(line) = serde_json::to_string(s) {
                total += (line.len() + 1) as u64;
            }
        }
        self.bytes = total;
    }

    /// Insert a validated batch from `peer_key`. Latest-wins on
    /// `(peer_key, day_utc)`. Refuses if the file would grow past
    /// `MAX_TOTAL_BYTES`.
    pub fn put(&mut self, peer_key: &str, batch: EndorsementBatch)
        -> Result<(), EndorsementSinkError>
    {
        let stored = StoredEndorsement {
            peer_key: peer_key.to_string(),
            batch,
        };
        let key = (stored.peer_key.clone(), stored.batch.day_utc);
        let new_line = serde_json::to_string(&stored)
            .map_err(|e| EndorsementValidateError::BadJson(e))?;
        let new_size = (new_line.len() + 1) as u64;
        let prev_size = self.by_key.get(&key)
            .and_then(|s| serde_json::to_string(s).ok())
            .map(|l| (l.len() + 1) as u64)
            .unwrap_or(0);
        let projected = self.bytes.saturating_sub(prev_size).saturating_add(new_size);
        if projected > MAX_TOTAL_BYTES {
            return Err(EndorsementSinkError::OverCap);
        }
        self.by_key.insert(key, stored);
        self.bytes = projected;
        Ok(())
    }

    /// All endorsements as a flattened (author, target, score) iterator.
    /// Used by the C++ side to populate TrustLedger::setEndorsementsFrom.
    pub fn flatten(&self) -> impl Iterator<Item = (String, String, f32)> + '_ {
        self.by_key.values().flat_map(|s| {
            s.batch.entries.iter().map(move |e| {
                (s.peer_key.clone(), e.target.clone(), e.score)
            })
        })
    }

    /// Rewrite the entire endorsements.jsonl atomically (write to a
    /// sibling temp file, then rename).
    pub fn flush(&self) -> Result<(), EndorsementSinkError> {
        if let Some(parent) = self.path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        let tmp = self.path.with_extension("jsonl.tmp");
        {
            let mut f = OpenOptions::new()
                .create(true).truncate(true).write(true).open(&tmp)?;
            // Sort by (peer_key, day_utc) so flush output is deterministic
            // — easier diffs and tests.
            let mut keys: Vec<&(String, u32)> = self.by_key.keys().collect();
            keys.sort();
            for k in keys {
                let line = serde_json::to_string(&self.by_key[k])
                    .map_err(|e| EndorsementValidateError::BadJson(e))?;
                f.write_all(line.as_bytes())?;
                f.write_all(b"\n")?;
            }
            f.sync_all()?;
        }
        std::fs::rename(&tmp, &self.path)?;
        Ok(())
    }

    pub fn len(&self) -> usize { self.by_key.len() }
    pub fn is_empty(&self) -> bool { self.by_key.is_empty() }
}

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile::tempdir;

    fn sample_batch(day: u32, entries: Vec<(&str, f32)>) -> String {
        let entries_json: Vec<serde_json::Value> = entries.into_iter()
            .map(|(t, s)| serde_json::json!({"target": t, "score": s}))
            .collect();
        let v = serde_json::json!({
            "schema": 1,
            "kind":   "endorsements",
            "day_utc": day,
            "entries": entries_json,
        });
        serde_json::to_string(&v).unwrap()
    }

    fn good_pk(c: char) -> String { c.to_string().repeat(64) }

    #[test]
    fn validate_good_batch() {
        let raw = sample_batch(20250507, vec![
            (&good_pk('a'), 1.6),
            (&good_pk('b'), 0.9),
        ]);
        let b = validate_endorsement_shape(&raw).unwrap();
        assert_eq!(b.schema, 1);
        assert_eq!(b.kind, "endorsements");
        assert_eq!(b.entries.len(), 2);
    }

    #[test]
    fn rejects_too_large() {
        let big = "x".repeat(MAX_BATCH_BYTES + 1);
        let err = validate_endorsement_shape(&big).unwrap_err();
        assert!(matches!(err, EndorsementValidateError::TooLarge(_)));
    }

    #[test]
    fn rejects_unknown_fields() {
        let v = serde_json::json!({
            "schema": 1, "kind": "endorsements",
            "day_utc": 20250507, "entries": [],
            "extra_field": "smuggled",
        });
        let raw = serde_json::to_string(&v).unwrap();
        let err = validate_endorsement_shape(&raw).unwrap_err();
        assert!(matches!(err, EndorsementValidateError::BadJson(_)));
    }

    #[test]
    fn rejects_wrong_schema() {
        let v = serde_json::json!({
            "schema": 2, "kind": "endorsements",
            "day_utc": 20250507, "entries": [],
        });
        let raw = serde_json::to_string(&v).unwrap();
        let err = validate_endorsement_shape(&raw).unwrap_err();
        assert!(matches!(err, EndorsementValidateError::SchemaVersion(2)));
    }

    #[test]
    fn rejects_wrong_kind() {
        let v = serde_json::json!({
            "schema": 1, "kind": "feedback",
            "day_utc": 20250507, "entries": [],
        });
        let raw = serde_json::to_string(&v).unwrap();
        let err = validate_endorsement_shape(&raw).unwrap_err();
        assert!(matches!(err, EndorsementValidateError::BadKind(_)));
    }

    #[test]
    fn rejects_bad_day() {
        let raw = sample_batch(19990101, vec![]);
        let err = validate_endorsement_shape(&raw).unwrap_err();
        assert!(matches!(err, EndorsementValidateError::BadDayUtc(_)));
    }

    #[test]
    fn rejects_too_many_entries() {
        let mut entries = Vec::new();
        for i in 0..(MAX_ENTRIES_PER_BATCH + 1) {
            entries.push((format!("{:064x}", i), 1.0));
        }
        let entries: Vec<(&str, f32)> = entries.iter()
            .map(|(s, w)| (s.as_str(), *w)).collect();
        let raw = sample_batch(20250507, entries);
        let err = validate_endorsement_shape(&raw).unwrap_err();
        assert!(matches!(err, EndorsementValidateError::TooManyEntries(_)));
    }

    #[test]
    fn rejects_uppercase_hex() {
        // Has uppercase letters — disallowed.
        let pk = "A".repeat(64);
        let raw = sample_batch(20250507, vec![(&pk, 1.0)]);
        let err = validate_endorsement_shape(&raw).unwrap_err();
        assert!(matches!(err, EndorsementValidateError::BadPubkey { .. }));
    }

    #[test]
    fn rejects_bad_pubkey_length() {
        let pk = "abc";
        let raw = sample_batch(20250507, vec![(pk, 1.0)]);
        let err = validate_endorsement_shape(&raw).unwrap_err();
        assert!(matches!(err, EndorsementValidateError::BadPubkey { .. }));
    }

    #[test]
    fn rejects_score_out_of_range() {
        let raw = sample_batch(20250507, vec![(&good_pk('a'), 5.0)]);
        let err = validate_endorsement_shape(&raw).unwrap_err();
        assert!(matches!(err, EndorsementValidateError::BadScore { .. }));
    }

    #[test]
    fn rejects_negative_score() {
        let raw = sample_batch(20250507, vec![(&good_pk('a'), -0.1)]);
        let err = validate_endorsement_shape(&raw).unwrap_err();
        assert!(matches!(err, EndorsementValidateError::BadScore { .. }));
    }

    #[test]
    fn rejects_duplicate_target() {
        let raw = sample_batch(20250507, vec![
            (&good_pk('a'), 1.0),
            (&good_pk('a'), 1.5),
        ]);
        let err = validate_endorsement_shape(&raw).unwrap_err();
        assert!(matches!(err, EndorsementValidateError::DuplicateTarget { .. }));
    }

    #[test]
    fn sink_round_trip() {
        let dir = tempdir().unwrap();
        let path = dir.path().join("endorsements.jsonl");
        let mut sink = EndorsementSink::open(&path).unwrap();
        assert!(sink.is_empty());

        let raw = sample_batch(20250507, vec![
            (&good_pk('a'), 1.6), (&good_pk('b'), 0.9),
        ]);
        let batch = validate_endorsement_shape(&raw).unwrap();
        sink.put(&good_pk('z'), batch).unwrap();
        sink.flush().unwrap();

        let sink2 = EndorsementSink::open(&path).unwrap();
        assert_eq!(sink2.len(), 1);
        let flat: Vec<_> = sink2.flatten().collect();
        assert_eq!(flat.len(), 2);
    }

    #[test]
    fn sink_dedup_same_peer_same_day() {
        let dir = tempdir().unwrap();
        let path = dir.path().join("endorsements.jsonl");
        let mut sink = EndorsementSink::open(&path).unwrap();
        let day = 20250507;

        let raw1 = sample_batch(day, vec![(&good_pk('a'), 1.0)]);
        sink.put(&good_pk('z'),
                 validate_endorsement_shape(&raw1).unwrap()).unwrap();

        let raw2 = sample_batch(day, vec![
            (&good_pk('a'), 1.5), (&good_pk('b'), 1.6),
        ]);
        sink.put(&good_pk('z'),
                 validate_endorsement_shape(&raw2).unwrap()).unwrap();

        // Latest wins: only 1 stored row (same peer/day); 2 entries flat.
        assert_eq!(sink.len(), 1);
        let flat: Vec<_> = sink.flatten().collect();
        assert_eq!(flat.len(), 2);
        // The score must be from raw2 (1.5 or 1.6), not 1.0.
        for (_a, _t, score) in &flat {
            assert!(*score >= 1.5);
        }
    }

    #[test]
    fn sink_keeps_distinct_days_separately() {
        let dir = tempdir().unwrap();
        let path = dir.path().join("endorsements.jsonl");
        let mut sink = EndorsementSink::open(&path).unwrap();
        let raw1 = sample_batch(20250507, vec![(&good_pk('a'), 1.0)]);
        let raw2 = sample_batch(20250508, vec![(&good_pk('b'), 1.0)]);
        sink.put(&good_pk('z'), validate_endorsement_shape(&raw1).unwrap()).unwrap();
        sink.put(&good_pk('z'), validate_endorsement_shape(&raw2).unwrap()).unwrap();
        assert_eq!(sink.len(), 2);
    }

    #[test]
    fn sink_keeps_distinct_peers_separately() {
        let dir = tempdir().unwrap();
        let path = dir.path().join("endorsements.jsonl");
        let mut sink = EndorsementSink::open(&path).unwrap();
        let day = 20250507;
        let raw = sample_batch(day, vec![(&good_pk('c'), 1.0)]);
        sink.put(&good_pk('a'), validate_endorsement_shape(&raw).unwrap()).unwrap();
        sink.put(&good_pk('b'), validate_endorsement_shape(&raw).unwrap()).unwrap();
        assert_eq!(sink.len(), 2);
    }
}
