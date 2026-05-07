//! Filesystem layout for the edit-pack repository.
//!
//! Storage shape:
//!     <data-dir>/
//!         <film-fp>/                    # 64-hex film fingerprint
//!             <author-pubkey>.json       # one pack per (film, author)
//!
//! Why (fp, author) keys: a single film can have packs from multiple
//! publishers (e.g. one pack that's "edits for under-5", another for
//! "under-9"); replacing your own pack overwrites your own file but
//! never clobbers anyone else's. SHA-256 hex / hex pubkeys are
//! filesystem-safe everywhere, so no escaping is needed.

use std::path::{Path, PathBuf};

use anyhow::{Context, Result};
use walkdir::WalkDir;

use crate::schema::{validate_shape, EditPack, PackError};

#[derive(Debug, thiserror::Error)]
pub enum StorageError {
    #[error("io: {0}")]
    Io(#[from] std::io::Error),
    #[error("schema: {0}")]
    Schema(#[from] PackError),
    #[error("pack's author/film keys disagree with its file path")]
    PathMismatch,
}

pub struct Repo {
    pub data_dir: PathBuf,
}

impl Repo {
    pub fn open(data_dir: &Path) -> Result<Self> {
        std::fs::create_dir_all(data_dir)
            .with_context(|| format!("create_dir_all {}", data_dir.display()))?;
        Ok(Self { data_dir: data_dir.to_path_buf() })
    }

    pub fn pack_path(&self, film_fp: &str, author_pubkey: &str) -> PathBuf {
        self.data_dir.join(film_fp).join(format!("{}.json", author_pubkey))
    }

    /// Write a pack, replacing any existing pack from the same author
    /// for the same film. The caller is responsible for having already
    /// validated the pack via `schema::validate_shape` and
    /// `schema::verify_signature`.
    pub fn put(&self, pack: &EditPack) -> Result<PathBuf, StorageError> {
        let dir = self.data_dir.join(&pack.film_fp);
        std::fs::create_dir_all(&dir)?;
        let path = self.pack_path(&pack.film_fp, &pack.author_pubkey);
        let json = serde_json::to_vec(pack)
            .map_err(|e| StorageError::Io(std::io::Error::new(std::io::ErrorKind::Other, e)))?;

        // Atomic temp+rename.
        let tmp = path.with_extension("json.part");
        std::fs::write(&tmp, &json)?;
        std::fs::rename(&tmp, &path)?;
        Ok(path)
    }

    /// Read every pack for the given fingerprint. Skips files that no
    /// longer parse cleanly (rather than aborting the whole listing).
    pub fn list_for_fingerprint(&self, film_fp: &str) -> Result<Vec<EditPack>, StorageError> {
        let dir = self.data_dir.join(film_fp);
        if !dir.exists() { return Ok(Vec::new()); }
        let mut out = Vec::new();
        for entry in std::fs::read_dir(&dir)? {
            let entry = entry?;
            let path = entry.path();
            if path.extension().and_then(|s| s.to_str()) != Some("json") { continue; }
            let raw = match std::fs::read_to_string(&path) {
                Ok(s)  => s,
                Err(_) => continue,
            };
            match validate_shape(&raw) {
                Ok(p)  => {
                    if p.film_fp == film_fp { out.push(p); }
                }
                Err(_) => continue,
            }
        }
        Ok(out)
    }

    /// Walk every pack in the repo. Used by the 'check' subcommand.
    pub fn walk(&self) -> impl Iterator<Item = PathBuf> + '_ {
        WalkDir::new(&self.data_dir)
            .into_iter()
            .filter_map(Result::ok)
            .filter(|e| e.file_type().is_file())
            .map(|e| e.into_path())
            .filter(|p| p.extension().and_then(|s| s.to_str()) == Some("json"))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::schema::{canonical_signed_bytes, EditPack, PackAnchor, PackCut};
    use chrono::TimeZone;
    use ed25519_dalek::{Signer, SigningKey};
    use rand::rngs::OsRng;
    use tempfile::tempdir;

    fn pack_with_author(film_fp: &str, sk: &SigningKey) -> EditPack {
        let mut p = EditPack {
            schema:        1,
            film_fp:       film_fp.to_string(),
            film_anchors:  vec![PackAnchor {
                tau: 0.10, phash: "1111aaaa00001111".to_string(),
            }],
            author_pubkey: hex::encode(sk.verifying_key().to_bytes()),
            created_utc:   chrono::Utc.with_ymd_and_hms(2026, 1, 1, 0, 0, 0).unwrap(),
            cuts:          vec![PackCut {
                start_ms: 1_000, end_ms: 2_000,
                category: "Cruelty".to_string(),
                kind: None, score: Some(0.8), reason: None,
            }],
            comment:       None,
            sig:           None,
        };
        // Sign.
        use base64::Engine;
        let bytes = canonical_signed_bytes(&p).unwrap();
        let sig = sk.sign(&bytes);
        p.sig = Some(base64::engine::general_purpose::STANDARD.encode(sig.to_bytes()));
        p
    }

    #[test]
    fn put_then_list_round_trip() {
        let dir = tempdir().unwrap();
        let repo = Repo::open(dir.path()).unwrap();
        let sk1 = SigningKey::generate(&mut OsRng);
        let sk2 = SigningKey::generate(&mut OsRng);
        let film = "a".repeat(64);
        let p1 = pack_with_author(&film, &sk1);
        let p2 = pack_with_author(&film, &sk2);
        repo.put(&p1).unwrap();
        repo.put(&p2).unwrap();
        let listed = repo.list_for_fingerprint(&film).unwrap();
        assert_eq!(listed.len(), 2);
        let pubkeys: std::collections::HashSet<_> =
            listed.iter().map(|p| p.author_pubkey.clone()).collect();
        assert!(pubkeys.contains(&p1.author_pubkey));
        assert!(pubkeys.contains(&p2.author_pubkey));
    }

    #[test]
    fn put_overwrites_same_author() {
        let dir = tempdir().unwrap();
        let repo = Repo::open(dir.path()).unwrap();
        let sk = SigningKey::generate(&mut OsRng);
        let film = "b".repeat(64);
        let mut p = pack_with_author(&film, &sk);
        repo.put(&p).unwrap();
        // Replace with a newer pack from the same author.
        p.created_utc = p.created_utc + chrono::Duration::days(1);
        // re-sign over the new bytes
        use base64::Engine;
        p.sig = None;
        let bytes = canonical_signed_bytes(&p).unwrap();
        let sig = sk.sign(&bytes);
        p.sig = Some(base64::engine::general_purpose::STANDARD.encode(sig.to_bytes()));
        repo.put(&p).unwrap();
        let listed = repo.list_for_fingerprint(&film).unwrap();
        assert_eq!(listed.len(), 1);
    }

    #[test]
    fn list_skips_corrupt_files() {
        let dir = tempdir().unwrap();
        let repo = Repo::open(dir.path()).unwrap();
        let sk = SigningKey::generate(&mut OsRng);
        let film = "c".repeat(64);
        let p = pack_with_author(&film, &sk);
        repo.put(&p).unwrap();
        // Drop a junk file alongside.
        let jpath = dir.path().join(&film).join("notapack.json");
        std::fs::write(&jpath, "}}}}garbage").unwrap();
        let listed = repo.list_for_fingerprint(&film).unwrap();
        assert_eq!(listed.len(), 1);
    }

    #[test]
    fn list_empty_for_unknown_fingerprint() {
        let dir = tempdir().unwrap();
        let repo = Repo::open(dir.path()).unwrap();
        let listed = repo.list_for_fingerprint(&"d".repeat(64)).unwrap();
        assert!(listed.is_empty());
    }
}
