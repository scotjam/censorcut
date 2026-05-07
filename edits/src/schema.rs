//! Edit-pack schema + signing/validation.
//!
//! An EditPack is a signed bundle of cut timestamps for one film,
//! identified by its 4-anchor audio fingerprint (M8.1). The signature
//! covers a canonical, deterministic byte sequence over every field
//! EXCEPT `sig` itself, so a valid pack can't have its film/author/
//! cuts/comment tampered with after publishing.
//!
//! Every untrusted boundary — POST /v1/edits, file load, P2P receive
//! once we have it — funnels through `validate_pack` which enforces
//! schema + size + author allowlist + signature verification.

use std::collections::HashSet;

use serde::{Deserialize, Serialize};

/// Hard ceiling on the serialized size of one pack.
pub const MAX_PACK_BYTES: usize = 64 * 1024;

/// Cap on the number of cut entries per pack.
pub const MAX_CUTS: usize = 500;

/// Cap on the number of fingerprint anchors carried with the pack.
/// Equal to the M8.1 anchor count for now (4) but kept as a constant
/// so a future fingerprint variant can bump it.
pub const MAX_ANCHORS: usize = 8;

/// Cap on category-name length and per-cut human comment length.
pub const MAX_CATEGORY_LEN: usize = 64;
pub const MAX_REASON_LEN:   usize = 512;
pub const MAX_COMMENT_LEN:  usize = 4096;

/// Returns true if the character is in our category-name allowlist
/// (alphanumeric ASCII + space, slash, hyphen, period). Mirrors
/// sync/src/schema.rs so peers and edit packs share the same rules.
pub fn is_allowed_category_char(c: char) -> bool {
    c.is_ascii_alphanumeric() || matches!(c, ' ' | '/' | '-' | '.')
}

/// Anchor descriptor we keep with the pack so receivers can verify
/// the pack is indexed against THEIR copy of the film, not just any
/// film with the same digest.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct PackAnchor {
    pub t_ms:     i64,
    pub peak_lufs: f32,
    /// 16-char hex; matches the M8.1 spectral signature shape.
    pub sig:      String,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum CutKind {
    /// Confirmed by the publisher — clients should treat as
    /// "trusted" and merge into their list.
    Confirmed,
    /// Suggested but not confirmed by the publisher; clients should
    /// treat as a starting point only.
    Suggested,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct PackCut {
    pub start_ms:  i64,
    pub end_ms:    i64,
    pub category:  String,
    #[serde(default)]
    pub kind:      Option<CutKind>,
    #[serde(default)]
    pub score:     Option<f32>,
    #[serde(default)]
    pub reason:    Option<String>,
}

/// Top-level edit pack record.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct EditPack {
    pub schema:       u32,
    /// SHA-256 hex of the film fingerprint (matches AnalysisResult.fingerprint.digest).
    pub film_fp:      String,
    /// Anchors used to index the pack — needed for time-alignment
    /// across slightly different versions of the film.
    pub film_anchors: Vec<PackAnchor>,
    /// Hex ed25519 pubkey of whoever signed the pack.
    pub author_pubkey: String,
    /// ISO-8601 UTC creation time. Helps receivers prefer fresher packs
    /// when multiple authors have packs for the same film.
    #[serde(with = "chrono::serde::ts_seconds")]
    pub created_utc:  chrono::DateTime<chrono::Utc>,
    pub cuts:         Vec<PackCut>,
    #[serde(default)]
    pub comment:      Option<String>,
    /// Base64 ed25519 signature over the canonical bytes of the pack
    /// with `sig` set to None.
    #[serde(default)]
    pub sig:          Option<String>,
}

#[derive(Debug, thiserror::Error)]
pub enum PackError {
    #[error("pack too large: {0} bytes")]
    TooLarge(usize),
    #[error("malformed JSON: {0}")]
    BadJson(serde_json::Error),
    #[error("unsupported schema {0}")]
    BadSchema(u32),
    #[error("film_fp must be 64 hex chars, got {0}")]
    BadFilmFp(usize),
    #[error("author_pubkey must be 64 hex chars, got {0}")]
    BadAuthorKey(usize),
    #[error("missing or empty signature")]
    MissingSig,
    #[error("signature verification failed")]
    BadSig,
    #[error("author key not in allowlist")]
    NotAllowedAuthor,
    #[error("cuts list exceeds {MAX_CUTS} entries")]
    TooManyCuts,
    #[error("anchors list exceeds {MAX_ANCHORS} entries")]
    TooManyAnchors,
    #[error("invalid anchor: {0}")]
    BadAnchor(String),
    #[error("invalid cut: {0}")]
    BadCut(String),
    #[error("comment too long ({0} > {MAX_COMMENT_LEN})")]
    CommentTooLong(usize),
}

/// Strip the signature, render canonically, and return the bytes the
/// signature is supposed to cover. Two well-formed packs that only
/// differ in `sig` produce IDENTICAL canonical bytes here, which is
/// what makes signing+verifying deterministic.
pub fn canonical_signed_bytes(pack: &EditPack) -> serde_json::Result<Vec<u8>> {
    let mut clone = pack.clone();
    clone.sig = None;
    // serde_json::to_vec emits keys in struct order, with no whitespace —
    // deterministic across runs and OSes for our types.
    serde_json::to_vec(&clone)
}

/// Strict structural validation. Doesn't verify the signature — that
/// happens in `verify_signature` once we know the author key is
/// allowed.
pub fn validate_shape(raw: &str) -> Result<EditPack, PackError> {
    if raw.len() > MAX_PACK_BYTES {
        return Err(PackError::TooLarge(raw.len()));
    }
    let pack: EditPack = serde_json::from_str(raw).map_err(PackError::BadJson)?;
    if pack.schema != 1 {
        return Err(PackError::BadSchema(pack.schema));
    }
    if pack.film_fp.len() != 64 || hex::decode(&pack.film_fp).is_err() {
        return Err(PackError::BadFilmFp(pack.film_fp.len()));
    }
    if pack.author_pubkey.len() != 64 || hex::decode(&pack.author_pubkey).is_err() {
        return Err(PackError::BadAuthorKey(pack.author_pubkey.len()));
    }
    if pack.film_anchors.len() > MAX_ANCHORS {
        return Err(PackError::TooManyAnchors);
    }
    for a in &pack.film_anchors {
        if a.sig.len() != 16 || hex::decode(&a.sig).is_err() {
            return Err(PackError::BadAnchor(format!("sig must be 16 hex, got {}", a.sig.len())));
        }
        if a.t_ms < 0 {
            return Err(PackError::BadAnchor("negative t_ms".to_string()));
        }
        if !a.peak_lufs.is_finite() {
            return Err(PackError::BadAnchor("non-finite peak_lufs".to_string()));
        }
    }
    if pack.cuts.len() > MAX_CUTS {
        return Err(PackError::TooManyCuts);
    }
    for c in &pack.cuts {
        if c.start_ms < 0 || c.end_ms <= c.start_ms {
            return Err(PackError::BadCut(format!("range [{},{}]", c.start_ms, c.end_ms)));
        }
        if c.category.is_empty() || c.category.chars().count() > MAX_CATEGORY_LEN
            || !c.category.chars().all(is_allowed_category_char)
        {
            return Err(PackError::BadCut(format!("category {:?}", c.category)));
        }
        if let Some(s) = c.score {
            if !s.is_finite() || !(0.0..=1.0).contains(&s) {
                return Err(PackError::BadCut(format!("score {s}")));
            }
        }
        if let Some(r) = &c.reason {
            if r.chars().count() > MAX_REASON_LEN {
                return Err(PackError::BadCut(format!("reason too long ({} chars)", r.chars().count())));
            }
        }
    }
    if let Some(co) = &pack.comment {
        if co.chars().count() > MAX_COMMENT_LEN {
            return Err(PackError::CommentTooLong(co.chars().count()));
        }
    }
    Ok(pack)
}

/// Verify the pack's signature using its declared author key. Caller
/// must check the allowlist separately (see `verify_against_allowlist`).
pub fn verify_signature(pack: &EditPack) -> Result<(), PackError> {
    use base64::Engine;
    use ed25519_dalek::{Signature, Verifier, VerifyingKey, SIGNATURE_LENGTH};
    let sig_b64 = pack.sig.as_deref().ok_or(PackError::MissingSig)?;
    let sig_bytes = base64::engine::general_purpose::STANDARD
        .decode(sig_b64.as_bytes())
        .map_err(|_| PackError::BadSig)?;
    if sig_bytes.len() != SIGNATURE_LENGTH {
        return Err(PackError::BadSig);
    }
    let mut sig_arr = [0u8; SIGNATURE_LENGTH];
    sig_arr.copy_from_slice(&sig_bytes);
    let signature = Signature::from_bytes(&sig_arr);

    let pub_bytes = hex::decode(&pack.author_pubkey).map_err(|_| PackError::BadAuthorKey(0))?;
    if pub_bytes.len() != 32 { return Err(PackError::BadAuthorKey(pub_bytes.len())); }
    let mut pub_arr = [0u8; 32];
    pub_arr.copy_from_slice(&pub_bytes);
    let vk = VerifyingKey::from_bytes(&pub_arr).map_err(|_| PackError::BadSig)?;

    let bytes = canonical_signed_bytes(pack).map_err(|_| PackError::BadSig)?;
    vk.verify(&bytes, &signature).map_err(|_| PackError::BadSig)?;
    Ok(())
}

/// True if the author key is in the supplied allowlist.
pub fn is_author_allowed(pack: &EditPack, allow: &HashSet<String>) -> bool {
    allow.is_empty() || allow.contains(&pack.author_pubkey)
}

#[cfg(test)]
mod tests {
    use super::*;
    use chrono::TimeZone;
    use ed25519_dalek::{Signer, SigningKey, SECRET_KEY_LENGTH};
    use rand::rngs::OsRng;

    fn good_anchor(t_ms: i64, sig: &str) -> PackAnchor {
        PackAnchor { t_ms, peak_lufs: -10.0, sig: sig.to_string() }
    }

    fn good_pack() -> EditPack {
        EditPack {
            schema:        1,
            film_fp:       "0".repeat(64),
            film_anchors:  vec![
                good_anchor(600_000,  "1111aaaa00001111"),
                good_anchor(1_200_000,"2222bbbb00002222"),
                good_anchor(4_500_000,"3333cccc00003333"),
                good_anchor(5_100_000,"4444dddd00004444"),
            ],
            author_pubkey: "0".repeat(64),
            created_utc:   chrono::Utc.with_ymd_and_hms(2026, 1, 1, 0, 0, 0).unwrap(),
            cuts:          vec![PackCut {
                start_ms: 1_000_000,
                end_ms:   1_002_000,
                category: "Cruelty".to_string(),
                kind:     Some(CutKind::Confirmed),
                score:    Some(0.8),
                reason:   Some("hitting".to_string()),
            }],
            comment:       Some("test".to_string()),
            sig:           None,
        }
    }

    fn fresh_keypair() -> SigningKey {
        SigningKey::generate(&mut OsRng)
    }

    fn sign(pack: &mut EditPack, sk: &SigningKey) {
        use base64::Engine;
        let bytes = canonical_signed_bytes(pack).unwrap();
        let sig = sk.sign(&bytes);
        pack.sig = Some(base64::engine::general_purpose::STANDARD.encode(sig.to_bytes()));
    }

    #[test]
    fn validate_shape_accepts_well_formed() {
        let mut p = good_pack();
        let sk = fresh_keypair();
        p.author_pubkey = hex::encode(sk.verifying_key().to_bytes());
        sign(&mut p, &sk);
        let raw = serde_json::to_string(&p).unwrap();
        assert_eq!(validate_shape(&raw).unwrap().film_fp, p.film_fp);
    }

    #[test]
    fn rejects_bad_schema() {
        let mut p = good_pack();
        p.schema = 5;
        let raw = serde_json::to_string(&p).unwrap();
        assert!(matches!(validate_shape(&raw), Err(PackError::BadSchema(5))));
    }

    #[test]
    fn rejects_bad_fingerprint_or_author() {
        let mut p = good_pack();
        p.film_fp = "abc".to_string();
        let raw = serde_json::to_string(&p).unwrap();
        assert!(matches!(validate_shape(&raw), Err(PackError::BadFilmFp(_))));

        let mut p = good_pack();
        p.author_pubkey = "not hex!".repeat(8);
        let raw = serde_json::to_string(&p).unwrap();
        assert!(matches!(validate_shape(&raw), Err(PackError::BadAuthorKey(_))));
    }

    #[test]
    fn rejects_bad_cut_range() {
        let mut p = good_pack();
        p.cuts[0].end_ms = p.cuts[0].start_ms;
        let raw = serde_json::to_string(&p).unwrap();
        assert!(matches!(validate_shape(&raw), Err(PackError::BadCut(_))));
    }

    #[test]
    fn rejects_bad_category() {
        let mut p = good_pack();
        p.cuts[0].category = "<script>".to_string();
        let raw = serde_json::to_string(&p).unwrap();
        assert!(matches!(validate_shape(&raw), Err(PackError::BadCut(_))));
    }

    #[test]
    fn rejects_too_many_cuts() {
        let mut p = good_pack();
        p.cuts = (0..(MAX_CUTS + 1) as i64).map(|i| PackCut {
            start_ms: i * 1000,
            end_ms:   i * 1000 + 100,
            category: "Cruelty".to_string(),
            kind: None, score: None, reason: None,
        }).collect();
        let raw = serde_json::to_string(&p).unwrap();
        assert!(matches!(validate_shape(&raw), Err(PackError::TooManyCuts)));
    }

    #[test]
    fn rejects_extra_fields() {
        let mut v = serde_json::to_value(good_pack()).unwrap();
        v["payload"] = serde_json::json!("free hosting");
        let raw = v.to_string();
        assert!(matches!(validate_shape(&raw), Err(PackError::BadJson(_))));
    }

    #[test]
    fn signature_round_trip() {
        let sk = fresh_keypair();
        let mut p = good_pack();
        p.author_pubkey = hex::encode(sk.verifying_key().to_bytes());
        sign(&mut p, &sk);
        verify_signature(&p).unwrap();
    }

    #[test]
    fn rejects_tampered_field() {
        let sk = fresh_keypair();
        let mut p = good_pack();
        p.author_pubkey = hex::encode(sk.verifying_key().to_bytes());
        sign(&mut p, &sk);
        // Tamper with film_fp after signing — verify must catch it.
        p.film_fp = "f".repeat(64);
        assert!(matches!(verify_signature(&p), Err(PackError::BadSig)));
    }

    #[test]
    fn rejects_wrong_author_key() {
        let sk1 = fresh_keypair();
        let sk2 = fresh_keypair();
        let mut p = good_pack();
        p.author_pubkey = hex::encode(sk1.verifying_key().to_bytes());
        sign(&mut p, &sk1);
        p.author_pubkey = hex::encode(sk2.verifying_key().to_bytes());
        assert!(matches!(verify_signature(&p), Err(PackError::BadSig)));
    }

    #[test]
    fn rejects_missing_signature() {
        let p = good_pack();
        assert!(matches!(verify_signature(&p), Err(PackError::MissingSig)));
    }

    #[test]
    fn allowlist_empty_means_open() {
        let allow = HashSet::new();
        let p = good_pack();
        assert!(is_author_allowed(&p, &allow));
    }

    #[test]
    fn allowlist_enforces_membership() {
        let p = good_pack();
        let mut allow = HashSet::new();
        allow.insert("ff".repeat(32));
        assert!(!is_author_allowed(&p, &allow));
        allow.insert(p.author_pubkey.clone());
        assert!(is_author_allowed(&p, &allow));
    }
}
