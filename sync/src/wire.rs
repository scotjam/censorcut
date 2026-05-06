//! Wire envelope for gossiped feedback rows.
//!
//! Each broadcast on the topic is one JSON line:
//!
//!     { "r": "<canonical FeedbackRow JSON>",
//!       "s": "<base64 ed25519 signature over r's bytes>",
//!       "k": "<hex ed25519 public key>" }
//!
//! Receivers must:
//!   1. Parse the envelope.
//!   2. Verify `s` against the literal bytes of `r` using `k`.
//!   3. Hand `r` to the same shape validation the local
//!      sink uses (no sneaking different rules onto the wire).
//!
//! The signature covers `r`'s exact byte sequence — *not* a re-serialized
//! version of the parsed row — so peers can't intercept and rewrite the
//! payload while keeping the signature valid. Anyone tweaking even
//! whitespace inside `r` invalidates the signature.

use serde::{Deserialize, Serialize};

use crate::crypto::{verify_b64, Identity, SignError};

/// Cap on the envelope's serialized size — leaves headroom for the
/// signature/key overhead beyond the 16 KB row cap.
pub const MAX_ENVELOPE_BYTES: usize = 24 * 1024;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Envelope {
    /// Raw FeedbackRow JSON, kept as a string so we can hash/verify
    /// over its canonical bytes rather than a re-encoded round-trip.
    #[serde(rename = "r")] pub row_json:  String,
    #[serde(rename = "s")] pub sig_b64:   String,
    #[serde(rename = "k")] pub pub_hex:   String,
}

#[derive(Debug, thiserror::Error)]
pub enum EnvelopeError {
    #[error("envelope too large: {0} bytes")]
    TooLarge(usize),
    #[error("malformed envelope: {0}")]
    BadJson(serde_json::Error),
    #[error("signature/key invalid: {0}")]
    BadSig(#[from] SignError),
}

/// Build an outbound envelope: sign the supplied row JSON bytes with
/// our identity and wrap them.
pub fn seal(row_json: &str, id: &Identity) -> Envelope {
    let sig_b64 = id.sign_b64(row_json.as_bytes());
    Envelope {
        row_json: row_json.to_string(),
        sig_b64,
        pub_hex: id.public_hex(),
    }
}

/// Validate envelope size + JSON shape + signature. Returns the
/// inner row JSON string and the peer's public key hex on success.
/// The caller is responsible for the row-level shape validation
/// (`schema::validate_row_shape`) and for routing through the sink.
pub fn open(raw: &str) -> Result<(String, String), EnvelopeError> {
    if raw.len() > MAX_ENVELOPE_BYTES {
        return Err(EnvelopeError::TooLarge(raw.len()));
    }
    let env: Envelope = serde_json::from_str(raw).map_err(EnvelopeError::BadJson)?;
    verify_b64(env.row_json.as_bytes(), &env.sig_b64, &env.pub_hex)?;
    Ok((env.row_json, env.pub_hex))
}

/// Convenience: serialize an Envelope into a JSON line (no trailing
/// newline so the caller can decide line terminators).
pub fn to_line(env: &Envelope) -> Result<String, serde_json::Error> {
    serde_json::to_string(env)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::crypto::Identity;

    fn good_row_json() -> String {
        // Hand-rolled to keep canonical bytes stable across runs.
        r#"{"schema":1,"category":"Cruelty","decision":"reject","score":0.5,"vec":[1,0,0]}"#.to_string()
    }

    #[test]
    fn round_trip() {
        let id = Identity::generate();
        let row = good_row_json();
        let env = seal(&row, &id);
        let line = to_line(&env).unwrap();
        let (got_row, got_pub) = open(&line).unwrap();
        assert_eq!(got_row, row);
        assert_eq!(got_pub, id.public_hex());
    }

    #[test]
    fn rejects_oversized() {
        let big = "{".repeat(MAX_ENVELOPE_BYTES + 1);
        let err = open(&big).unwrap_err();
        assert!(matches!(err, EnvelopeError::TooLarge(_)));
    }

    #[test]
    fn rejects_unknown_fields_in_envelope() {
        // deny_unknown_fields stops a peer from piggybacking arbitrary
        // metadata on the envelope (free remote storage attempt).
        let id = Identity::generate();
        let env = seal(&good_row_json(), &id);
        let mut value = serde_json::to_value(&env).unwrap();
        value["payload"] = serde_json::json!("hi mom");
        let raw = serde_json::to_string(&value).unwrap();
        let err = open(&raw).unwrap_err();
        assert!(matches!(err, EnvelopeError::BadJson(_)));
    }

    #[test]
    fn rejects_tampered_row() {
        let id = Identity::generate();
        let row = good_row_json();
        let env = seal(&row, &id);
        // Change a byte in `r` while leaving the signature alone.
        let mut tampered = env.clone();
        tampered.row_json = tampered.row_json.replace("0.5", "0.9");
        let raw = to_line(&tampered).unwrap();
        let err = open(&raw).unwrap_err();
        assert!(matches!(err, EnvelopeError::BadSig(_)));
    }

    #[test]
    fn rejects_wrong_pubkey() {
        let id1 = Identity::generate();
        let id2 = Identity::generate();
        let env = seal(&good_row_json(), &id1);
        let mut bad = env.clone();
        bad.pub_hex = id2.public_hex();
        let raw = to_line(&bad).unwrap();
        let err = open(&raw).unwrap_err();
        assert!(matches!(err, EnvelopeError::BadSig(_)));
    }
}
