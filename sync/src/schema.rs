//! Strict schema for feedback rows.
//!
//! Anything that crosses the network is funnelled through `validate_row`.
//! The validator is paranoid by design: any deviation from the expected
//! shape, range, or size — including unknown fields — rejects the row.
//! That gives us a predictable filter for both denial-of-service attacks
//! ("flood with huge or bogus rows") and any attempt to repurpose the
//! channel as remote storage for arbitrary data.

use serde::{Deserialize, Serialize};

/// Hard ceiling on a single row's serialized JSON size, including the
/// trailing newline. Prevents a peer from forcing us to allocate gigabytes
/// for one absurd vector.
pub const MAX_ROW_BYTES: usize = 16 * 1024;

/// L2-norm tolerance — accept the row if `|‖vec‖ - 1.0| <= L2_TOL`.
pub const L2_TOLERANCE: f32 = 0.05;

/// Per-element value cap. CLIP / SigLIP normalized features stay well
/// inside this range; anything outside is malformed.
pub const VEC_VALUE_LIMIT: f32 = 1.5;

/// Cap on the length of a category name, in chars (not bytes). Keeps
/// the wire size predictable and stops anyone smuggling kilobytes of
/// payload through a "category" field.
pub const MAX_CATEGORY_LEN: usize = 64;

/// Whether a single character is permitted in a category name. The
/// rule is "alphanumeric + a tiny safe-punctuation whitelist": ASCII
/// letters, ASCII digits, and the four symbols our built-in names
/// already use ('Crying / sad', 'Hitting / impacts'): space, slash,
/// hyphen, period. Anything else — Unicode oddities, control chars,
/// brackets, quotes, backslashes — is rejected.
fn is_allowed_category_char(c: char) -> bool {
    c.is_ascii_alphanumeric() ||
    matches!(c, ' ' | '/' | '-' | '.')
}

/// Acceptable vector dimensions. Currently the CLIP variants we support
/// produce 512 / 768 / 1024-dim feature vectors.
pub const VALID_VEC_DIMS: &[usize] = &[512, 768, 1024];

/// The 12 built-in categories shipped with the editor. Peers can't
/// invent new namespaces silently — anything outside the configured
/// accepted-categories set is dropped by `validate_row`. The user can
/// extend the set by accepting custom categories in the editor (which
/// passes them in via `SchemaConfig`); we never auto-trust unknown
/// categories from the network.
pub const BUILTIN_CATEGORIES: &[&str] = &[
    "Jump scare",
    "Screaming",
    "Crying / sad",
    "Yelling",
    "Gunfire",
    "Hitting / impacts",
    "Sword fight",
    "Chase",
    "Violence (general)",
    "Cruelty",
    "Pushing",
    "Kill / threat",
];

/// Runtime-tunable schema policy. The accepted-categories list comes from
/// the editor (built-ins + any custom categories the user has explicitly
/// approved in the Sharing settings); unknown categories on the wire are
/// rejected before they hit disk.
#[derive(Debug, Clone)]
pub struct SchemaConfig {
    pub accepted_categories: Vec<String>,
}

impl SchemaConfig {
    /// The minimum-trust default: just the 12 built-ins.
    pub fn builtin() -> Self {
        Self {
            accepted_categories: BUILTIN_CATEGORIES.iter().map(|s| s.to_string()).collect(),
        }
    }
}

impl Default for SchemaConfig {
    fn default() -> Self {
        Self::builtin()
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct FeedbackRow {
    /// Schema version — only 1 is currently accepted. Bumping this is a
    /// breaking change that needs to roll out before peers start sending
    /// the new shape.
    pub schema:   u32,
    pub category: String,
    pub decision: Decision,
    pub score:    f32,
    pub vec:      Vec<f32>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Decision {
    Accept,
    Reject,
}

#[derive(Debug, thiserror::Error)]
pub enum ValidateError {
    #[error("row exceeds {MAX_ROW_BYTES} bytes (was {0})")]
    TooLarge(usize),
    #[error("invalid JSON: {0}")]
    BadJson(serde_json::Error),
    #[error("unsupported schema {0}")]
    BadSchema(u32),
    #[error("category name {0:?} is too long (max {MAX_CATEGORY_LEN})")]
    CategoryTooLong(String),
    #[error("category name {0:?} contains a disallowed character")]
    CategoryBadChars(String),
    #[error("category {0:?} is not in the accepted set")]
    BadCategory(String),
    #[error("score {0} not in [0, 1]")]
    BadScore(f32),
    #[error("vector dim {0} not in {VALID_VEC_DIMS:?}")]
    BadVecDim(usize),
    #[error("vector entry #{idx} = {value} is non-finite or out of range")]
    BadVecValue { idx: usize, value: f32 },
    #[error("vector L2 norm {0} not within {L2_TOLERANCE} of 1.0")]
    NotNormalized(f32),
}

/// Pure-shape validation: schema version, byte size, category-name
/// SHAPE (chars + length), score range, vector dim/value/norm. Does NOT
/// check whether the category is in the user's accepted set — that's a
/// separate decision (`is_accepted_category`) so unknown-but-shapely
/// categories can be routed to the proposed pile rather than dropped.
pub fn validate_row_shape(raw: &str) -> Result<FeedbackRow, ValidateError> {
    if raw.len() > MAX_ROW_BYTES {
        return Err(ValidateError::TooLarge(raw.len()));
    }
    let row: FeedbackRow = serde_json::from_str(raw).map_err(ValidateError::BadJson)?;
    if row.schema != 1 {
        return Err(ValidateError::BadSchema(row.schema));
    }
    if row.category.chars().count() > MAX_CATEGORY_LEN {
        return Err(ValidateError::CategoryTooLong(row.category));
    }
    if row.category.is_empty()
        || !row.category.chars().all(is_allowed_category_char)
    {
        return Err(ValidateError::CategoryBadChars(row.category));
    }
    if !(0.0..=1.0).contains(&row.score) || !row.score.is_finite() {
        return Err(ValidateError::BadScore(row.score));
    }
    if !VALID_VEC_DIMS.contains(&row.vec.len()) {
        return Err(ValidateError::BadVecDim(row.vec.len()));
    }
    let mut sumsq: f64 = 0.0;
    for (idx, &v) in row.vec.iter().enumerate() {
        if !v.is_finite() || v.abs() > VEC_VALUE_LIMIT {
            return Err(ValidateError::BadVecValue { idx, value: v });
        }
        sumsq += (v as f64) * (v as f64);
    }
    let norm = sumsq.sqrt() as f32;
    if (norm - 1.0).abs() > L2_TOLERANCE {
        return Err(ValidateError::NotNormalized(norm));
    }
    Ok(row)
}

/// Whether a category appears in the user's accepted set (built-ins +
/// any custom names they've explicitly approved via the Sharing UI).
pub fn is_accepted_category(name: &str, cfg: &SchemaConfig) -> bool {
    cfg.accepted_categories.iter().any(|c| c == name)
}

/// Convenience: full validation including accepted-set membership.
/// Equivalent to `validate_row_shape` followed by an `is_accepted_category`
/// check. Kept for tests and CLI tools that don't separate the two
/// stages — production sink path uses the two helpers individually so it
/// can route unknown-but-shapely categories to the proposed pile.
pub fn validate_row(raw: &str, cfg: &SchemaConfig) -> Result<FeedbackRow, ValidateError> {
    let row = validate_row_shape(raw)?;
    if !is_accepted_category(&row.category, cfg) {
        return Err(ValidateError::BadCategory(row.category));
    }
    Ok(row)
}

/// SHA-256 of the rounded float vector. Used to dedup messages so a peer
/// re-broadcasting the same row doesn't multiply its weight.
pub fn fingerprint(vec: &[f32]) -> String {
    use sha2::{Digest, Sha256};
    let mut h = Sha256::new();
    for v in vec {
        // Quantize to 4 decimal places so trivially perturbed copies of
        // the same vector still hash equal.
        let q = (v * 10_000.0).round() as i32;
        h.update(q.to_le_bytes());
    }
    hex::encode(h.finalize())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn norm_vec(n: usize) -> Vec<f32> {
        // Construct a unit vector: first entry 1.0, rest 0.0.
        let mut v = vec![0.0f32; n];
        v[0] = 1.0;
        v
    }

    fn good_row() -> FeedbackRow {
        FeedbackRow {
            schema:   1,
            category: "Cruelty".to_string(),
            decision: Decision::Reject,
            score:    0.85,
            vec:      norm_vec(768),
        }
    }

    fn round_trip(row: &FeedbackRow) -> String {
        serde_json::to_string(row).unwrap()
    }

    fn cfg() -> SchemaConfig { SchemaConfig::builtin() }

    #[test]
    fn accepts_a_well_formed_row() {
        let raw = round_trip(&good_row());
        assert_eq!(validate_row(&raw, &cfg()).unwrap(), good_row());
    }

    #[test]
    fn rejects_unknown_schema() {
        let mut r = good_row();
        r.schema = 99;
        let raw = round_trip(&r);
        assert!(matches!(validate_row(&raw, &cfg()), Err(ValidateError::BadSchema(99))));
    }

    #[test]
    fn rejects_unknown_category_by_default() {
        let mut r = good_row();
        r.category = "Made up category".to_string();
        let raw = round_trip(&r);
        assert!(matches!(validate_row(&raw, &cfg()), Err(ValidateError::BadCategory(_))));
    }

    #[test]
    fn rejects_category_with_disallowed_chars() {
        let mut r = good_row();
        r.category = "<script>alert(1)</script>".to_string();
        let raw = round_trip(&r);
        // Even if a user "added" this somehow, the char check fires first.
        let mut c = cfg();
        c.accepted_categories.push(r.category.clone());
        assert!(matches!(validate_row(&raw, &c), Err(ValidateError::CategoryBadChars(_))));
    }

    #[test]
    fn rejects_category_with_unicode_lookalikes() {
        let mut r = good_row();
        // Cyrillic letters that visually resemble Latin letters — common
        // homoglyph attack vector.
        r.category = "Сruelty".to_string();
        let raw = round_trip(&r);
        assert!(matches!(validate_row(&raw, &cfg()), Err(ValidateError::CategoryBadChars(_))));
    }

    #[test]
    fn rejects_category_too_long() {
        let mut r = good_row();
        r.category = "A".repeat(MAX_CATEGORY_LEN + 1);
        let raw = round_trip(&r);
        let mut c = cfg();
        c.accepted_categories.push(r.category.clone());
        assert!(matches!(validate_row(&raw, &c), Err(ValidateError::CategoryTooLong(_))));
    }

    #[test]
    fn rejects_empty_category() {
        let mut r = good_row();
        r.category = "".to_string();
        let raw = round_trip(&r);
        let mut c = cfg();
        c.accepted_categories.push("".to_string());
        assert!(matches!(validate_row(&raw, &c), Err(ValidateError::CategoryBadChars(_))));
    }

    #[test]
    fn accepts_user_added_category() {
        let mut r = good_row();
        r.category = "Vulgar gestures".to_string();
        let raw = round_trip(&r);
        let mut c = cfg();
        c.accepted_categories.push("Vulgar gestures".to_string());
        assert!(validate_row(&raw, &c).is_ok());
    }

    #[test]
    fn rejects_score_out_of_range() {
        let mut r = good_row();
        r.score = 1.5;
        let raw = round_trip(&r);
        assert!(matches!(validate_row(&raw, &cfg()), Err(ValidateError::BadScore(_))));
    }

    #[test]
    fn rejects_wrong_dim() {
        let mut r = good_row();
        r.vec = vec![1.0, 0.0, 0.0]; // dim 3 not in allowed list
        let raw = round_trip(&r);
        assert!(matches!(validate_row(&raw, &cfg()), Err(ValidateError::BadVecDim(3))));
    }

    #[test]
    fn rejects_unnormalized() {
        let mut r = good_row();
        r.vec = vec![2.0; 768];
        let raw = round_trip(&r);
        assert!(matches!(validate_row(&raw, &cfg()),
                         Err(ValidateError::BadVecValue { .. })));

        let mut r2 = good_row();
        r2.vec = vec![0.05; 768];
        let raw2 = round_trip(&r2);
        assert!(matches!(validate_row(&raw2, &cfg()), Err(ValidateError::NotNormalized(_))));
    }

    #[test]
    fn rejects_oversized_row() {
        let big = "x".repeat(MAX_ROW_BYTES + 100);
        let raw = serde_json::json!({
            "schema": 1,
            "category": big,
            "decision": "reject",
            "score": 0.5,
            "vec": vec![0.0_f32; 768],
        }).to_string();
        assert!(matches!(validate_row(&raw, &cfg()), Err(ValidateError::TooLarge(_))));
    }

    #[test]
    fn rejects_extra_fields() {
        let raw = serde_json::json!({
            "schema": 1,
            "category": "Cruelty",
            "decision": "reject",
            "score": 0.5,
            "vec": norm_vec(768),
            "remote_storage_payload": "free hosting plz",
        }).to_string();
        assert!(matches!(validate_row(&raw, &cfg()), Err(ValidateError::BadJson(_))));
    }

    #[test]
    fn fingerprint_is_stable_for_quantized_inputs() {
        let v1: Vec<f32> = (0..768).map(|i| (i as f32) * 1e-3).collect();
        let mut v2 = v1.clone();
        // Tiny per-element jitter under the quantization step shouldn't
        // change the fingerprint.
        for x in &mut v2 { *x += 1e-6; }
        assert_eq!(fingerprint(&v1), fingerprint(&v2));
    }

    #[test]
    fn fingerprint_differs_for_distinct_vectors() {
        let mut v = norm_vec(768);
        let f1 = fingerprint(&v);
        v[0] = 0.9;
        let f2 = fingerprint(&v);
        assert_ne!(f1, f2);
    }
}
