//! `peers.jsonl` sink: deduplicates, enforces a per-source rate limit,
//! and a global byte cap.
//!
//! Threat model:
//!   - A peer floods us with valid-looking rows hoping we'll either run
//!     out of disk or accept enough of their rows to bias our analyzer
//!     in their direction. The per-key daily cap mitigates the second;
//!     the total-byte cap mitigates the first.
//!   - A peer rebroadcasts its rows constantly to inflate apparent
//!     "weight". The fingerprint dedup prevents that.
//!   - A peer tries to use the file as remote storage by pushing rows
//!     full of garbage encoded as floats. `validate_row` already filters
//!     to a fixed schema with normalized embeddings; this layer merely
//!     adds the size discipline.

use std::collections::{HashMap, HashSet};
use std::fs::{File, OpenOptions};
use std::io::{BufRead, BufReader, BufWriter, Seek, SeekFrom, Write};
use std::path::Path;

use chrono::{Datelike, Utc};
use serde::{Deserialize, Serialize};

use crate::schema::{
    is_accepted_category, validate_row_shape, FeedbackRow, SchemaConfig, ValidateError,
};

/// One stored row in `peers.jsonl`. Wraps the validated payload with the
/// metadata our rate-limiter cares about: which peer's signing key
/// vouched for it, and a content fingerprint for dedup.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct StoredRow {
    pub peer_key:    String,   // hex-encoded ed25519 public key (16 chars min)
    pub fingerprint: String,   // sha256 of the rounded vector (see schema::fingerprint)
    pub day_utc:     u32,      // YYYYMMDD when this peer posted (rate-limit bucket)
    #[serde(flatten)]
    pub row:         FeedbackRow,
}

/// Tunables — values come from CLI flags or sensible defaults.
#[derive(Debug, Clone, Copy)]
pub struct SinkLimits {
    pub max_total_bytes:           u64,
    pub max_rows_per_peer_per_day: u32,
    /// Cap on the proposed-categories file (separate from the main
    /// peers file). Default smaller than the main cap because proposals
    /// are unverified.
    pub max_proposed_bytes:        u64,
    /// Hard ceiling on the number of distinct proposed category names
    /// we'll track at all — stops a flooder from dirtying the UI with
    /// thousands of fake categories.
    pub max_proposed_categories:   u32,
    /// Per-category cap on sample rows we keep under proposal. Once
    /// reached, additional rows for that category are dropped (the
    /// user has plenty to evaluate already).
    pub max_proposed_samples_per_category: u32,
}

impl Default for SinkLimits {
    fn default() -> Self {
        Self {
            max_total_bytes:           50 * 1024 * 1024,
            max_rows_per_peer_per_day: 200,
            max_proposed_bytes:        5 * 1024 * 1024,
            max_proposed_categories:   100,
            max_proposed_samples_per_category: 50,
        }
    }
}

#[derive(Debug, thiserror::Error)]
pub enum SinkError {
    #[error("validation failed: {0}")]
    Validate(#[from] ValidateError),
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),
    #[error("peer-day rate limit exceeded for {peer_key} on {day_utc}")]
    PeerLimit { peer_key: String, day_utc: u32 },
    #[error("file would exceed cap of {cap} bytes")]
    OverCap { cap: u64 },
    #[error("duplicate row (fingerprint already stored)")]
    Duplicate,
}

/// Reasons a row got dropped — used for diagnostics, not for control flow.
#[derive(Debug, Default, Clone)]
pub struct AcceptStats {
    pub accepted:           u64,
    pub deduped:            u64,
    pub rate_limited:       u64,
    pub over_cap:           u64,
    pub bad_format:         u64,
    pub proposed:           u64,
    pub proposed_dropped:   u64,
}

/// In-memory index over the on-disk peers.jsonl + proposed.jsonl files.
/// Rebuilt on startup.
///
/// `peers.jsonl`     — rows for accepted categories (full feedback the
///                      analyzer will actually consume).
/// `proposed.jsonl`  — rows for shapely-but-unaccepted categories
///                      (peers proposing a new namespace). The user
///                      decides whether to promote a category in the
///                      Sharing UI; on promotion the C++ side adds the
///                      name to the accepted set and copies the rows
///                      across.
pub struct PeerSink {
    accepted_path:      std::path::PathBuf,
    proposed_path:      Option<std::path::PathBuf>,
    limits:             SinkLimits,
    schema_cfg:         SchemaConfig,
    fingerprints:       HashSet<String>,
    daily_counts:       HashMap<(String, u32), u32>,
    bytes_on_disk:      u64,
    proposed_bytes:     u64,
    proposed_per_cat:   HashMap<String, u32>,
    pub stats:          AcceptStats,
}

impl PeerSink {
    pub fn open(path: &Path, limits: SinkLimits, schema_cfg: SchemaConfig)
        -> Result<Self, SinkError>
    {
        Self::open_with_proposed(path, None, limits, schema_cfg)
    }

    pub fn open_with_proposed(accepted_path: &Path,
                              proposed_path: Option<&Path>,
                              limits: SinkLimits,
                              schema_cfg: SchemaConfig)
        -> Result<Self, SinkError>
    {
        if let Some(parent) = accepted_path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        if let Some(p) = proposed_path {
            if let Some(parent) = p.parent() {
                std::fs::create_dir_all(parent)?;
            }
        }
        let mut sink = Self {
            accepted_path:    accepted_path.to_path_buf(),
            proposed_path:    proposed_path.map(|p| p.to_path_buf()),
            limits,
            schema_cfg,
            fingerprints:     HashSet::new(),
            daily_counts:     HashMap::new(),
            bytes_on_disk:    0,
            proposed_bytes:   0,
            proposed_per_cat: HashMap::new(),
            stats:            AcceptStats::default(),
        };
        sink.reindex()?;
        Ok(sink)
    }

    fn reindex(&mut self) -> Result<(), SinkError> {
        self.fingerprints.clear();
        self.daily_counts.clear();
        self.bytes_on_disk = 0;
        self.proposed_bytes = 0;
        self.proposed_per_cat.clear();

        if self.accepted_path.exists() {
            let f = File::open(&self.accepted_path)?;
            self.bytes_on_disk = f.metadata()?.len();
            for line in BufReader::new(f).lines() {
                let line = line?;
                if line.trim().is_empty() { continue; }
                let stored: StoredRow = match serde_json::from_str(&line) {
                    Ok(s)  => s,
                    Err(_) => continue,
                };
                self.fingerprints.insert(stored.fingerprint.clone());
                *self.daily_counts.entry((stored.peer_key, stored.day_utc)).or_insert(0) += 1;
            }
        }

        if let Some(pp) = &self.proposed_path {
            if pp.exists() {
                let f = File::open(pp)?;
                self.proposed_bytes = f.metadata()?.len();
                for line in BufReader::new(f).lines() {
                    let line = line?;
                    if line.trim().is_empty() { continue; }
                    let stored: StoredRow = match serde_json::from_str(&line) {
                        Ok(s)  => s,
                        Err(_) => continue,
                    };
                    self.fingerprints.insert(stored.fingerprint.clone());
                    *self.proposed_per_cat
                        .entry(stored.row.category).or_insert(0) += 1;
                }
            }
        }
        Ok(())
    }

    /// Validate + dedup + rate-limit + size-check + append. Routes:
    ///   - Shape-failing rows → dropped (bad_format counter).
    ///   - Shape-OK + accepted-category → peers.jsonl.
    ///   - Shape-OK + unknown-category → proposed.jsonl (if proposed
    ///     path is configured AND the per-category and global caps
    ///     allow another sample). Otherwise dropped.
    pub fn try_accept(&mut self, raw: &str, peer_key: &str)
        -> Result<StoredRow, SinkError>
    {
        let row = match validate_row_shape(raw) {
            Ok(r) => r,
            Err(e) => { self.stats.bad_format += 1; return Err(e.into()); }
        };

        let fingerprint = crate::schema::fingerprint(&row.vec);
        if self.fingerprints.contains(&fingerprint) {
            self.stats.deduped += 1;
            return Err(SinkError::Duplicate);
        }

        let now = Utc::now();
        let day_utc = (now.year() as u32) * 10_000
                     + (now.month()       ) * 100
                     +  now.day();
        let key = (peer_key.to_string(), day_utc);
        let count = *self.daily_counts.get(&key).unwrap_or(&0);
        if count >= self.limits.max_rows_per_peer_per_day {
            self.stats.rate_limited += 1;
            return Err(SinkError::PeerLimit { peer_key: peer_key.to_string(), day_utc });
        }

        let accepted_category = is_accepted_category(&row.category, &self.schema_cfg);
        let stored = StoredRow {
            peer_key:    peer_key.to_string(),
            fingerprint: fingerprint.clone(),
            day_utc,
            row,
        };
        let mut serialized = serde_json::to_string(&stored)
            .map_err(|e| SinkError::Io(std::io::Error::new(std::io::ErrorKind::Other, e)))?;
        serialized.push('\n');
        let row_bytes = serialized.len() as u64;

        if accepted_category {
            if self.bytes_on_disk + row_bytes > self.limits.max_total_bytes {
                self.stats.over_cap += 1;
                return Err(SinkError::OverCap { cap: self.limits.max_total_bytes });
            }
            let f = OpenOptions::new().create(true).append(true).open(&self.accepted_path)?;
            let mut w = BufWriter::new(f);
            w.write_all(serialized.as_bytes())?;
            w.flush()?;
            self.fingerprints.insert(fingerprint);
            *self.daily_counts.entry(key).or_insert(0) += 1;
            self.bytes_on_disk += row_bytes;
            self.stats.accepted += 1;
            return Ok(stored);
        }

        // Unknown but shape-valid category → propose. Multiple caps so
        // a flooder can't waste disk OR clutter the user's UI with fake
        // category names.
        let Some(pp) = self.proposed_path.clone() else {
            self.stats.proposed_dropped += 1;
            return Err(SinkError::OverCap { cap: 0 });
        };
        let cur = *self.proposed_per_cat.get(&stored.row.category).unwrap_or(&0);
        if cur >= self.limits.max_proposed_samples_per_category {
            self.stats.proposed_dropped += 1;
            return Err(SinkError::OverCap {
                cap: self.limits.max_proposed_samples_per_category as u64
            });
        }
        if cur == 0
            && self.proposed_per_cat.len() as u32 >= self.limits.max_proposed_categories
        {
            self.stats.proposed_dropped += 1;
            return Err(SinkError::OverCap {
                cap: self.limits.max_proposed_categories as u64
            });
        }
        if self.proposed_bytes + row_bytes > self.limits.max_proposed_bytes {
            self.stats.proposed_dropped += 1;
            return Err(SinkError::OverCap { cap: self.limits.max_proposed_bytes });
        }
        let f = OpenOptions::new().create(true).append(true).open(&pp)?;
        let mut w = BufWriter::new(f);
        w.write_all(serialized.as_bytes())?;
        w.flush()?;
        self.fingerprints.insert(fingerprint);
        *self.proposed_per_cat.entry(stored.row.category.clone()).or_insert(0) += 1;
        self.proposed_bytes += row_bytes;
        self.stats.proposed += 1;
        Ok(stored)
    }

    pub fn bytes_on_disk(&self)      -> u64    { self.bytes_on_disk }
    pub fn proposed_bytes(&self)     -> u64    { self.proposed_bytes }
    pub fn fingerprint_count(&self)  -> usize  { self.fingerprints.len() }
    pub fn proposed_categories(&self) -> &HashMap<String, u32> { &self.proposed_per_cat }
}

/// Strict appender for the LOCAL feedback file. Same byte cap, same
/// validation — the only difference is that we don't rate-limit
/// ourselves. Used by the sync subprocess to write its own decisions if
/// it ever needs to (it currently only reads the local file produced by
/// the C++ side, but having symmetric helpers keeps tests honest).
pub fn truncate_to_size(path: &Path, max_bytes: u64) -> std::io::Result<()> {
    let f = match OpenOptions::new().read(true).write(true).open(path) {
        Ok(f) => f,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(()),
        Err(e) => return Err(e),
    };
    let len = f.metadata()?.len();
    if len <= max_bytes { return Ok(()); }
    // Drop the oldest lines until we're under the cap.
    let reader = BufReader::new(&f);
    let mut keep: Vec<String> = Vec::new();
    let mut keep_bytes: u64 = 0;
    let lines: Vec<String> = reader.lines().filter_map(Result::ok).collect();
    for line in lines.iter().rev() {
        let n = line.len() as u64 + 1;
        if keep_bytes + n > max_bytes { break; }
        keep.push(line.clone());
        keep_bytes += n;
    }
    drop(f);
    let mut f = OpenOptions::new()
        .write(true).truncate(true).create(true)
        .open(path)?;
    f.seek(SeekFrom::Start(0))?;
    for line in keep.iter().rev() {
        writeln!(f, "{}", line)?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::schema::{Decision, FeedbackRow, SchemaConfig};
    use tempfile::tempdir;

    fn unit_vec(n: usize, slot: usize) -> Vec<f32> {
        let mut v = vec![0.0f32; n];
        v[slot] = 1.0;
        v
    }
    fn raw_row(idx: usize) -> String {
        let row = FeedbackRow {
            schema:   1,
            category: "Cruelty".to_string(),
            decision: Decision::Reject,
            score:    0.7,
            vec:      unit_vec(768, idx % 768),
        };
        serde_json::to_string(&row).unwrap()
    }
    fn cfg() -> SchemaConfig { SchemaConfig::builtin() }

    #[test]
    fn accepts_valid_then_dedups() {
        let dir = tempdir().unwrap();
        let path = dir.path().join("peers.jsonl");
        let mut sink = PeerSink::open(&path, SinkLimits::default(), cfg()).unwrap();
        sink.try_accept(&raw_row(0), "peerA").unwrap();
        let err = sink.try_accept(&raw_row(0), "peerA").unwrap_err();
        assert!(matches!(err, SinkError::Duplicate));
        assert_eq!(sink.stats.accepted, 1);
        assert_eq!(sink.stats.deduped, 1);
    }

    #[test]
    fn enforces_per_peer_daily_limit() {
        let dir = tempdir().unwrap();
        let path = dir.path().join("peers.jsonl");
        let limits = SinkLimits { max_rows_per_peer_per_day: 3, ..Default::default() };
        let mut sink = PeerSink::open(&path, limits, cfg()).unwrap();
        for i in 0..3 {
            sink.try_accept(&raw_row(i), "peerB").unwrap();
        }
        let err = sink.try_accept(&raw_row(99), "peerB").unwrap_err();
        assert!(matches!(err, SinkError::PeerLimit { .. }));
        // A different peer is unaffected by peerB's quota.
        sink.try_accept(&raw_row(50), "peerC").unwrap();
    }

    #[test]
    fn enforces_global_byte_cap() {
        let dir = tempdir().unwrap();
        let path = dir.path().join("peers.jsonl");
        // Pick a very small cap so the second row trips it.
        let limits = SinkLimits { max_total_bytes: 4_000, ..Default::default() };
        let mut sink = PeerSink::open(&path, limits, cfg()).unwrap();
        sink.try_accept(&raw_row(0), "peerA").unwrap();
        let err = sink.try_accept(&raw_row(1), "peerA").unwrap_err();
        assert!(matches!(err, SinkError::OverCap { .. }));
    }

    #[test]
    fn rejects_invalid_rows_without_polluting_state() {
        let dir = tempdir().unwrap();
        let path = dir.path().join("peers.jsonl");
        let mut sink = PeerSink::open(&path, SinkLimits::default(), cfg()).unwrap();
        let bad = r#"{"schema":1,"category":"Cruelty","decision":"reject","score":2.0,"vec":[1.0]}"#;
        let err = sink.try_accept(bad, "peerA").unwrap_err();
        assert!(matches!(err, SinkError::Validate(_)));
        assert_eq!(sink.stats.bad_format, 1);
        assert_eq!(sink.fingerprint_count(), 0);
        assert_eq!(sink.bytes_on_disk(), 0);
    }

    fn proposed_row(idx: usize, name: &str) -> String {
        let row = FeedbackRow {
            schema:   1,
            category: name.to_string(),
            decision: Decision::Reject,
            score:    0.65,
            vec:      unit_vec(768, idx % 768),
        };
        serde_json::to_string(&row).unwrap()
    }

    #[test]
    fn unknown_category_routes_to_proposed_when_path_set() {
        let dir = tempdir().unwrap();
        let p_accepted = dir.path().join("peers.jsonl");
        let p_proposed = dir.path().join("proposed.jsonl");
        let mut sink = PeerSink::open_with_proposed(
            &p_accepted, Some(&p_proposed), SinkLimits::default(), cfg(),
        ).unwrap();
        let stored = sink.try_accept(&proposed_row(0, "Bullying"), "peerA").unwrap();
        assert_eq!(stored.row.category, "Bullying");
        assert_eq!(sink.stats.proposed, 1);
        assert!(p_proposed.exists());
        assert_eq!(sink.proposed_categories().get("Bullying").copied(), Some(1));
        // Accepted file untouched.
        assert!(!p_accepted.exists() || std::fs::metadata(&p_accepted).unwrap().len() == 0);
    }

    #[test]
    fn unknown_category_dropped_when_no_proposed_path() {
        let dir = tempdir().unwrap();
        let p = dir.path().join("peers.jsonl");
        let mut sink = PeerSink::open(&p, SinkLimits::default(), cfg()).unwrap();
        let err = sink.try_accept(&proposed_row(0, "Bullying"), "peerA").unwrap_err();
        assert!(matches!(err, SinkError::OverCap { .. }));
        assert_eq!(sink.stats.proposed_dropped, 1);
    }

    #[test]
    fn proposed_category_cap_blocks_new_names() {
        let dir = tempdir().unwrap();
        let p_accepted = dir.path().join("peers.jsonl");
        let p_proposed = dir.path().join("proposed.jsonl");
        let limits = SinkLimits { max_proposed_categories: 2, ..Default::default() };
        let mut sink = PeerSink::open_with_proposed(
            &p_accepted, Some(&p_proposed), limits, cfg(),
        ).unwrap();
        sink.try_accept(&proposed_row(0, "CatA"), "peerA").unwrap();
        sink.try_accept(&proposed_row(1, "CatB"), "peerA").unwrap();
        // Third NEW category is dropped, but additional samples for an
        // existing proposed category are still accepted up to the per-
        // category sample cap.
        let err = sink.try_accept(&proposed_row(2, "CatC"), "peerA").unwrap_err();
        assert!(matches!(err, SinkError::OverCap { .. }));
        sink.try_accept(&proposed_row(3, "CatA"), "peerA").unwrap();
    }

    #[test]
    fn proposed_per_category_sample_cap() {
        let dir = tempdir().unwrap();
        let p_accepted = dir.path().join("peers.jsonl");
        let p_proposed = dir.path().join("proposed.jsonl");
        let limits = SinkLimits {
            max_proposed_samples_per_category: 2,
            ..Default::default()
        };
        let mut sink = PeerSink::open_with_proposed(
            &p_accepted, Some(&p_proposed), limits, cfg(),
        ).unwrap();
        sink.try_accept(&proposed_row(0, "Bullying"), "peerA").unwrap();
        sink.try_accept(&proposed_row(1, "Bullying"), "peerA").unwrap();
        let err = sink.try_accept(&proposed_row(2, "Bullying"), "peerA").unwrap_err();
        assert!(matches!(err, SinkError::OverCap { .. }));
    }

    #[test]
    fn proposed_byte_cap() {
        let dir = tempdir().unwrap();
        let p_accepted = dir.path().join("peers.jsonl");
        let p_proposed = dir.path().join("proposed.jsonl");
        let limits = SinkLimits { max_proposed_bytes: 4_000, ..Default::default() };
        let mut sink = PeerSink::open_with_proposed(
            &p_accepted, Some(&p_proposed), limits, cfg(),
        ).unwrap();
        sink.try_accept(&proposed_row(0, "Bullying"), "peerA").unwrap();
        let err = sink.try_accept(&proposed_row(1, "Bullying"), "peerA").unwrap_err();
        assert!(matches!(err, SinkError::OverCap { .. }));
    }

    #[test]
    fn proposed_dedups_against_accepted() {
        // A row whose vector matches an already-accepted row should be
        // dropped as a duplicate even if its category is a fresh
        // proposal — same fingerprint, same content.
        let dir = tempdir().unwrap();
        let p_accepted = dir.path().join("peers.jsonl");
        let p_proposed = dir.path().join("proposed.jsonl");
        let mut sink = PeerSink::open_with_proposed(
            &p_accepted, Some(&p_proposed), SinkLimits::default(), cfg(),
        ).unwrap();
        sink.try_accept(&raw_row(0), "peerA").unwrap();
        let err = sink.try_accept(&proposed_row(0, "Bullying"), "peerA").unwrap_err();
        assert!(matches!(err, SinkError::Duplicate));
    }

    #[test]
    fn reindex_picks_up_pre_existing_rows() {
        let dir = tempdir().unwrap();
        let path = dir.path().join("peers.jsonl");
        {
            let mut sink = PeerSink::open(&path, SinkLimits::default(), cfg()).unwrap();
            sink.try_accept(&raw_row(0), "peerA").unwrap();
            sink.try_accept(&raw_row(1), "peerA").unwrap();
        }
        // Re-open: dedup table should already be populated from disk.
        let mut sink2 = PeerSink::open(&path, SinkLimits::default(), cfg()).unwrap();
        assert_eq!(sink2.fingerprint_count(), 2);
        let err = sink2.try_accept(&raw_row(0), "peerA").unwrap_err();
        assert!(matches!(err, SinkError::Duplicate));
    }
}
