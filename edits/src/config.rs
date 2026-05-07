//! Server configuration: the publisher allowlist file.
//!
//! Format: newline-separated 64-char hex ed25519 public keys. Lines
//! starting with '#' are comments; whitespace-only lines are ignored.
//! Bad lines (wrong length, non-hex) are skipped with a warning so a
//! single typo doesn't cause the server to refuse to start.

use std::collections::HashSet;
use std::fs;
use std::path::Path;

/// Read the allowlist file. Returns the parsed set of pubkeys (hex
/// strings, lowercased) and a count of malformed lines that were
/// skipped. An empty / missing file means "no allowlist" — every
/// signature-valid pack is accepted (use --bind 127.0.0.1 to keep
/// access local in that case).
pub fn load_allowlist(path: &Path) -> std::io::Result<(HashSet<String>, usize)> {
    let mut out = HashSet::new();
    let mut skipped = 0usize;
    if !path.exists() {
        return Ok((out, 0));
    }
    let text = fs::read_to_string(path)?;
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') { continue; }
        // Strip inline comments after '#'.
        let key = match line.split_once('#') {
            Some((before, _)) => before.trim(),
            None              => line,
        };
        if key.len() != 64 || hex::decode(key).is_err() {
            eprintln!("censorcut-edits: ignoring invalid allowlist line {:?}", key);
            skipped += 1;
            continue;
        }
        out.insert(key.to_lowercase());
    }
    Ok((out, skipped))
}

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile::tempdir;

    fn write(p: &Path, body: &str) {
        fs::write(p, body).unwrap();
    }

    #[test]
    fn empty_file_yields_empty_set() {
        let dir = tempdir().unwrap();
        let p = dir.path().join("a.txt");
        write(&p, "");
        let (set, skipped) = load_allowlist(&p).unwrap();
        assert!(set.is_empty());
        assert_eq!(skipped, 0);
    }

    #[test]
    fn missing_file_yields_empty_set() {
        let dir = tempdir().unwrap();
        let p = dir.path().join("nope.txt");
        let (set, skipped) = load_allowlist(&p).unwrap();
        assert!(set.is_empty());
        assert_eq!(skipped, 0);
    }

    #[test]
    fn parses_valid_keys_and_skips_bad_ones() {
        let dir = tempdir().unwrap();
        let p = dir.path().join("a.txt");
        let body = format!(
            "# comment line\n\
             {a}\n\
             \n\
             {b}  # alice\n\
             not a hex line\n\
             {c}",
            a = "a".repeat(64),
            b = "b".repeat(64),
            c = "z".repeat(64),  // not hex
        );
        write(&p, &body);
        let (set, skipped) = load_allowlist(&p).unwrap();
        assert!(set.contains(&"a".repeat(64)));
        assert!(set.contains(&"b".repeat(64)));
        assert_eq!(skipped, 2);
    }

    #[test]
    fn lowercases_keys_for_case_insensitive_match() {
        let dir = tempdir().unwrap();
        let p = dir.path().join("a.txt");
        write(&p, "AABBCCDDEEFF11223344556677889900AABBCCDDEEFF11223344556677889900");
        let (set, _) = load_allowlist(&p).unwrap();
        assert!(set.contains(&"aabbccddeeff11223344556677889900aabbccddeeff11223344556677889900".to_string()));
    }
}
