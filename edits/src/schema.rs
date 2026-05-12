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

/// Hard ceiling on the serialized size of one pack. Sized to fit a
/// keyframes fingerprint (2000 i64 timestamps ~ 18 KB JSON) plus the
/// cuts list, author key, signature, and any comment with headroom.
pub const MAX_PACK_BYTES: usize = 128 * 1024;

/// Cap on the number of cut entries per pack.
pub const MAX_CUTS: usize = 500;

/// Cap on the number of keyframe timestamps embedded in a F fingerprint.
/// 20k accommodates very long films with dense keyframes (200 min film
/// at 1 keyframe / sec = 12k entries). Matches the C++-side cap in
/// AnalysisResult.cpp.
pub const MAX_KEYFRAME_TIMES: usize = 20_000;

/// Cap on the number of audio peaks embedded in a v9 fingerprint.
/// Matches the v9 detector's K=25 default with headroom.
pub const MAX_PEAKS: usize = 200;

/// Cap on the bucket-key length. The producer typically uses
/// approxDurationMin as a stringified integer (3-4 chars). Keep the
/// cap small to make filesystem paths bounded.
pub const MAX_FILM_ID_LEN: usize = 32;

/// Cap on category-name length and per-cut human comment length.
pub const MAX_CATEGORY_LEN: usize = 64;
pub const MAX_REASON_LEN:   usize = 512;
pub const MAX_COMMENT_LEN:  usize = 4096;

/// Type-tag values for embedded fingerprint. Mirrors the C++ namespace
/// fp_type::* and the Python video_fingerprint module's type field.
pub const FP_TYPE_KEYFRAMES:       &str = "keyframes";
pub const FP_TYPE_AUDIO_PEAK_GAPS: &str = "audio_peak_gaps";

/// Returns true if the character is in our category-name allowlist
/// (alphanumeric ASCII + space, slash, hyphen, period). Mirrors
/// sync/src/schema.rs so peers and edit packs share the same rules.
pub fn is_allowed_category_char(c: char) -> bool {
    c.is_ascii_alphanumeric() || matches!(c, ' ' | '/' | '-' | '.')
}

/// One peak in a v9-fallback fingerprint. `t_ms` is the absolute time
/// of the audio peak; `phash` is the averaged-5-frame pHash at that
/// time (16 hex chars = 64 bits). May be empty when pHash decode
/// failed for that peak.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct PackPeak {
    pub t_ms:  i64,
    pub phash: String,
}

/// Embedded fingerprint carried with each pack so receivers can verify
/// the pack is indexed against THEIR copy of the film, not just any
/// film in the same duration bucket. Shape mirrors the producer's
/// FilmFingerprint exactly so the C++ side parses it via the same
/// helper used for analyzer output.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct EmbeddedFingerprint {
    pub version: u32,
    /// One of FP_TYPE_KEYFRAMES or FP_TYPE_AUDIO_PEAK_GAPS.
    #[serde(rename = "type")]
    pub fp_type: String,
    #[serde(rename = "durationMs")]
    pub duration_ms: i64,
    #[serde(rename = "approxDurationMin")]
    pub approx_duration_min: i32,

    // Keyframes variant (omitted for the other type)
    #[serde(rename = "keyframeTimesMs", default,
            skip_serializing_if = "Vec::is_empty")]
    pub keyframe_times_ms: Vec<i64>,

    // AudioPeakGaps variant (omitted for the other type)
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub peaks: Vec<PackPeak>,
    #[serde(rename = "gapsMs", default, skip_serializing_if = "Vec::is_empty")]
    pub gaps_ms: Vec<i64>,
    #[serde(rename = "innerSpanMs", default, skip_serializing_if = "is_zero")]
    pub inner_span_ms: i64,
}

fn is_zero(n: &i64) -> bool { *n == 0 }

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
    /// Bucket key the server indexes by. Typically the publisher's
    /// approxDurationMin as a stringified integer (e.g. "87"). Loose
    /// allowlist: ASCII alphanumeric, `-`, `_`. The fingerprint below
    /// is what the receiver actually verifies against.
    pub film_id:      String,
    /// Producer's full fingerprint. Receivers run their local matcher
    /// against this to decide whether the pack's cuts apply to their
    /// copy of the film.
    pub fingerprint:  EmbeddedFingerprint,
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
    #[error("film_id invalid: {0}")]
    BadFilmId(String),
    #[error("fingerprint invalid: {0}")]
    BadFingerprint(String),
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
    #[error("invalid cut: {0}")]
    BadCut(String),
    #[error("comment too long ({0} > {MAX_COMMENT_LEN})")]
    CommentTooLong(usize),
}

/// True iff `s` is a safe film_id (bucket key) — non-empty, length-
/// capped, ASCII alphanumeric + `-` / `_`. Restrictive on purpose so
/// the id is filesystem-safe (used as a directory name in storage)
/// and can't smuggle path-traversal sequences.
pub fn is_valid_film_id(s: &str) -> bool {
    if s.is_empty() || s.len() > MAX_FILM_ID_LEN { return false; }
    s.chars().all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_')
}

fn validate_fingerprint(fp: &EmbeddedFingerprint) -> Result<(), PackError> {
    if fp.fp_type != FP_TYPE_KEYFRAMES && fp.fp_type != FP_TYPE_AUDIO_PEAK_GAPS {
        return Err(PackError::BadFingerprint(
            format!("type must be {:?} or {:?}, got {:?}",
                    FP_TYPE_KEYFRAMES, FP_TYPE_AUDIO_PEAK_GAPS, fp.fp_type)));
    }
    if fp.duration_ms <= 0 {
        return Err(PackError::BadFingerprint(
            format!("durationMs must be positive, got {}", fp.duration_ms)));
    }
    if fp.fp_type == FP_TYPE_KEYFRAMES {
        if fp.keyframe_times_ms.is_empty() {
            return Err(PackError::BadFingerprint(
                "keyframes variant requires non-empty keyframeTimesMs".into()));
        }
        if fp.keyframe_times_ms.len() > MAX_KEYFRAME_TIMES {
            return Err(PackError::BadFingerprint(
                format!("keyframeTimesMs exceeds {} entries", MAX_KEYFRAME_TIMES)));
        }
        // Timestamps must be sorted ascending and within [0, duration_ms].
        // Allow a 1-second overshoot at the tail to absorb ffprobe vs
        // container-duration disagreements.
        let mut prev: i64 = -1;
        for &t in &fp.keyframe_times_ms {
            if t < 0 || t > fp.duration_ms + 1000 {
                return Err(PackError::BadFingerprint(
                    format!("keyframe time {t} out of [0, durationMs+1000]")));
            }
            if t < prev {
                return Err(PackError::BadFingerprint(
                    "keyframeTimesMs not sorted ascending".into()));
            }
            prev = t;
        }
        // peaks must be empty for the keyframes variant.
        if !fp.peaks.is_empty() || !fp.gaps_ms.is_empty() {
            return Err(PackError::BadFingerprint(
                "keyframes variant must not carry peaks / gapsMs".into()));
        }
    } else {
        if fp.peaks.is_empty() {
            return Err(PackError::BadFingerprint(
                "audio_peak_gaps variant requires non-empty peaks".into()));
        }
        if fp.peaks.len() > MAX_PEAKS {
            return Err(PackError::BadFingerprint(
                format!("peaks exceeds {MAX_PEAKS} entries")));
        }
        for p in &fp.peaks {
            if p.t_ms < 0 || p.t_ms > fp.duration_ms + 1000 {
                return Err(PackError::BadFingerprint(
                    format!("peak t_ms {} out of [0, durationMs+1000]", p.t_ms)));
            }
            if !p.phash.is_empty()
                && (p.phash.len() != 16 || hex::decode(&p.phash).is_err())
            {
                return Err(PackError::BadFingerprint(
                    format!("peak phash must be 16 hex or empty, got {:?}",
                            p.phash)));
            }
        }
        // keyframeTimesMs must be empty for the audio variant.
        if !fp.keyframe_times_ms.is_empty() {
            return Err(PackError::BadFingerprint(
                "audio_peak_gaps variant must not carry keyframeTimesMs".into()));
        }
    }
    Ok(())
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
    if !is_valid_film_id(&pack.film_id) {
        return Err(PackError::BadFilmId(pack.film_id.clone()));
    }
    if pack.author_pubkey.len() != 64 || hex::decode(&pack.author_pubkey).is_err() {
        return Err(PackError::BadAuthorKey(pack.author_pubkey.len()));
    }
    validate_fingerprint(&pack.fingerprint)?;
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

    /// 60 monotonically increasing keyframe times that pass
    /// validate_fingerprint's checks.
    fn good_keyframes() -> Vec<i64> {
        (0..60).map(|i| (i * 1000) as i64).collect()
    }

    fn good_fingerprint() -> EmbeddedFingerprint {
        EmbeddedFingerprint {
            version:             1,
            fp_type:             FP_TYPE_KEYFRAMES.to_string(),
            duration_ms:         60_000,
            approx_duration_min: 1,
            keyframe_times_ms:   good_keyframes(),
            peaks:               vec![],
            gaps_ms:             vec![],
            inner_span_ms:       0,
        }
    }

    fn good_pack() -> EditPack {
        EditPack {
            schema:        1,
            film_id:       "87".to_string(),
            fingerprint:   good_fingerprint(),
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
        assert_eq!(validate_shape(&raw).unwrap().film_id, p.film_id);
    }

    #[test]
    fn rejects_bad_schema() {
        let mut p = good_pack();
        p.schema = 5;
        let raw = serde_json::to_string(&p).unwrap();
        assert!(matches!(validate_shape(&raw), Err(PackError::BadSchema(5))));
    }

    #[test]
    fn rejects_bad_film_id_or_author() {
        let mut p = good_pack();
        p.film_id = "../etc/passwd".to_string();
        let raw = serde_json::to_string(&p).unwrap();
        assert!(matches!(validate_shape(&raw), Err(PackError::BadFilmId(_))));

        let mut p = good_pack();
        p.film_id = "".to_string();
        let raw = serde_json::to_string(&p).unwrap();
        assert!(matches!(validate_shape(&raw), Err(PackError::BadFilmId(_))));

        let mut p = good_pack();
        p.author_pubkey = "not hex!".repeat(8);
        let raw = serde_json::to_string(&p).unwrap();
        assert!(matches!(validate_shape(&raw), Err(PackError::BadAuthorKey(_))));
    }

    #[test]
    fn rejects_bad_fingerprint_shape() {
        // type=keyframes but no keyframeTimesMs → fail
        let mut p = good_pack();
        p.fingerprint.keyframe_times_ms = vec![];
        let raw = serde_json::to_string(&p).unwrap();
        assert!(matches!(validate_shape(&raw), Err(PackError::BadFingerprint(_))));

        // type=audio_peak_gaps with peaks
        let mut p = good_pack();
        p.fingerprint = EmbeddedFingerprint {
            version: 1,
            fp_type: FP_TYPE_AUDIO_PEAK_GAPS.to_string(),
            duration_ms: 60_000,
            approx_duration_min: 1,
            keyframe_times_ms: vec![],
            peaks: vec![PackPeak { t_ms: 10_000, phash: "1111aaaa00001111".to_string() },
                          PackPeak { t_ms: 30_000, phash: "2222bbbb00002222".to_string() },
                          PackPeak { t_ms: 50_000, phash: "3333cccc00003333".to_string() }],
            gaps_ms: vec![],
            inner_span_ms: 0,
        };
        let sk = fresh_keypair();
        p.author_pubkey = hex::encode(sk.verifying_key().to_bytes());
        sign(&mut p, &sk);
        let raw = serde_json::to_string(&p).unwrap();
        assert!(validate_shape(&raw).is_ok());

        // Unsorted keyframes → fail
        let mut p = good_pack();
        p.fingerprint.keyframe_times_ms = vec![1000, 500, 2000];
        let raw = serde_json::to_string(&p).unwrap();
        assert!(matches!(validate_shape(&raw), Err(PackError::BadFingerprint(_))));
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
        // Tamper with film_id after signing — verify must catch it.
        p.film_id = "99".to_string();
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
