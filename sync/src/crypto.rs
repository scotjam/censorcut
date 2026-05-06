//! Ephemeral signing keys + signature primitives.
//!
//! On first run we generate a fresh ed25519 keypair and persist the
//! 32-byte secret seed under ~/.censorcut/identity.key. The PUBLIC key
//! is the only thing peers ever see (encoded hex in wire envelopes). The
//! secret never leaves disk.
//!
//! "Ephemeral" here means scoped to this installation — there's no
//! identity registry, no rotation policy, no link to the user. If the
//! user wipes the file (or deletes ~/.censorcut entirely) they get a
//! brand-new identity on next launch and lose the bookkeeping that
//! peers used to rate-limit them. That's intended: identity is just
//! glue for the gossip protocol's per-source caps, never an account.

use std::fs;
use std::io::Write;
use std::path::Path;

use ed25519_dalek::{
    Signature, Signer, SigningKey, Verifier, VerifyingKey,
    SECRET_KEY_LENGTH, SIGNATURE_LENGTH,
};
use rand::rngs::OsRng;

/// Length of the on-disk identity file (raw secret seed).
pub const IDENTITY_FILE_SIZE: usize = SECRET_KEY_LENGTH;

#[derive(Debug, thiserror::Error)]
pub enum KeyError {
    #[error("identity file is the wrong size (expected {expected}, got {actual})")]
    BadLength { expected: usize, actual: usize },
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),
}

#[derive(Debug, thiserror::Error)]
pub enum SignError {
    #[error("invalid signature length (expected {SIGNATURE_LENGTH}, got {0})")]
    BadSigLength(usize),
    #[error("invalid public key length (expected 32, got {0})")]
    BadPubKeyLength(usize),
    #[error("hex decode error: {0}")]
    Hex(#[from] hex::FromHexError),
    #[error("base64 decode error: {0}")]
    Base64(#[from] base64::DecodeError),
    #[error("signature verification failed")]
    BadSignature,
    #[error("malformed public key")]
    MalformedKey,
}

#[derive(Clone)]
pub struct Identity {
    signing: SigningKey,
}

impl Identity {
    /// Generate a fresh keypair from the OS RNG.
    pub fn generate() -> Self {
        Self { signing: SigningKey::generate(&mut OsRng) }
    }

    /// Load identity from disk; if the file is missing, create one.
    pub fn load_or_create(path: &Path) -> Result<Self, KeyError> {
        if path.exists() {
            return Self::load(path);
        }
        let me = Self::generate();
        me.save(path)?;
        Ok(me)
    }

    pub fn load(path: &Path) -> Result<Self, KeyError> {
        let bytes = fs::read(path)?;
        if bytes.len() != IDENTITY_FILE_SIZE {
            return Err(KeyError::BadLength {
                expected: IDENTITY_FILE_SIZE, actual: bytes.len(),
            });
        }
        let mut seed = [0u8; SECRET_KEY_LENGTH];
        seed.copy_from_slice(&bytes);
        Ok(Self { signing: SigningKey::from_bytes(&seed) })
    }

    pub fn save(&self, path: &Path) -> Result<(), KeyError> {
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)?;
        }
        // Write to a temp file then rename for atomicity. Ignore the
        // permission-tightening on Windows (no chmod) — Windows ACLs
        // already restrict the file to the current user under their
        // home directory.
        let tmp = path.with_extension("key.part");
        let mut f = fs::OpenOptions::new()
            .create(true).truncate(true).write(true).open(&tmp)?;
        f.write_all(self.signing.as_bytes())?;
        f.sync_all()?;
        drop(f);
        fs::rename(&tmp, path)?;
        // Best effort 0o600 on Unix.
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mut perms = fs::metadata(path)?.permissions();
            perms.set_mode(0o600);
            fs::set_permissions(path, perms)?;
        }
        #[cfg(not(unix))]
        let _ = path;
        Ok(())
    }

    /// Hex-encoded public key — the only thing peers see.
    pub fn public_hex(&self) -> String {
        hex::encode(self.signing.verifying_key().to_bytes())
    }

    /// Sign arbitrary bytes; returns base64-encoded signature for
    /// transport in JSON envelopes.
    pub fn sign_b64(&self, message: &[u8]) -> String {
        use base64::Engine;
        let sig: Signature = self.signing.sign(message);
        base64::engine::general_purpose::STANDARD.encode(sig.to_bytes())
    }
}

/// Verify a base64 signature against the supplied bytes using a
/// hex-encoded public key.
pub fn verify_b64(message: &[u8], sig_b64: &str, pub_hex: &str) -> Result<(), SignError> {
    use base64::Engine;
    let sig_bytes = base64::engine::general_purpose::STANDARD.decode(sig_b64.as_bytes())?;
    if sig_bytes.len() != SIGNATURE_LENGTH {
        return Err(SignError::BadSigLength(sig_bytes.len()));
    }
    let mut sig_arr = [0u8; SIGNATURE_LENGTH];
    sig_arr.copy_from_slice(&sig_bytes);
    let signature = Signature::from_bytes(&sig_arr);

    let pub_bytes = hex::decode(pub_hex)?;
    if pub_bytes.len() != 32 {
        return Err(SignError::BadPubKeyLength(pub_bytes.len()));
    }
    let mut pub_arr = [0u8; 32];
    pub_arr.copy_from_slice(&pub_bytes);
    let vk = VerifyingKey::from_bytes(&pub_arr).map_err(|_| SignError::MalformedKey)?;
    vk.verify(message, &signature).map_err(|_| SignError::BadSignature)
}

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile::tempdir;

    #[test]
    fn generated_identity_has_32_byte_pubkey() {
        let id = Identity::generate();
        assert_eq!(id.public_hex().len(), 64);
    }

    #[test]
    fn save_then_load_round_trips() {
        let dir = tempdir().unwrap();
        let path = dir.path().join("identity.key");
        let id1 = Identity::generate();
        id1.save(&path).unwrap();
        let id2 = Identity::load(&path).unwrap();
        assert_eq!(id1.public_hex(), id2.public_hex());
    }

    #[test]
    fn load_or_create_creates_when_missing() {
        let dir = tempdir().unwrap();
        let path = dir.path().join("identity.key");
        assert!(!path.exists());
        let _ = Identity::load_or_create(&path).unwrap();
        assert!(path.exists());
        assert_eq!(fs::metadata(&path).unwrap().len() as usize, IDENTITY_FILE_SIZE);
    }

    #[test]
    fn load_rejects_wrong_length_file() {
        let dir = tempdir().unwrap();
        let path = dir.path().join("identity.key");
        fs::write(&path, b"too short").unwrap();
        assert!(matches!(Identity::load(&path), Err(KeyError::BadLength { .. })));
    }

    #[test]
    fn sign_then_verify_passes() {
        let id = Identity::generate();
        let msg = b"hello, censorcut";
        let sig = id.sign_b64(msg);
        verify_b64(msg, &sig, &id.public_hex()).unwrap();
    }

    #[test]
    fn verify_fails_for_tampered_message() {
        let id = Identity::generate();
        let sig = id.sign_b64(b"original");
        let err = verify_b64(b"tampered", &sig, &id.public_hex()).unwrap_err();
        assert!(matches!(err, SignError::BadSignature));
    }

    #[test]
    fn verify_fails_for_wrong_key() {
        let id1 = Identity::generate();
        let id2 = Identity::generate();
        let msg = b"x";
        let sig = id1.sign_b64(msg);
        let err = verify_b64(msg, &sig, &id2.public_hex()).unwrap_err();
        assert!(matches!(err, SignError::BadSignature));
    }

    #[test]
    fn verify_rejects_malformed_inputs() {
        let id = Identity::generate();
        let msg = b"x";
        let sig = id.sign_b64(msg);
        // Wrong-length pubkey.
        let err = verify_b64(msg, &sig, "deadbeef").unwrap_err();
        assert!(matches!(err, SignError::BadPubKeyLength(_)));
        // Garbage signature base64.
        let err2 = verify_b64(msg, "!!not-base64!!", &id.public_hex()).unwrap_err();
        assert!(matches!(err2, SignError::Base64(_)));
    }
}

