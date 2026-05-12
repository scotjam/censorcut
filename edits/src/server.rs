//! HTTP routes for the local-first edit-pack repository.
//!
//! Exactly two endpoints; both go through the same shape + signature
//! + allowlist + size pipeline that the on-disk `check` subcommand
//! uses, so anything stored via the network is also valid offline.
//!
//! GET /v1/edits?id=<film-id>
//!     200 OK  {"packs": [EditPack, ...]}     — possibly empty
//!     400 BadRequest                          — id missing / malformed
//!     500 InternalServerError                 — disk error
//!
//! POST /v1/edits  body: EditPack JSON
//!     200 OK  {"stored": "<path>"}            — accepted + persisted
//!     400 BadRequest                          — schema/shape failure
//!     401 Unauthorized                        — missing / bad signature
//!     403 Forbidden                           — author not in allowlist
//!     413 PayloadTooLarge                     — body exceeds MAX_PACK_BYTES
//!     500 InternalServerError                 — disk error

use std::collections::HashSet;
use std::sync::Arc;

use axum::{
    extract::{Query, State},
    http::StatusCode,
    response::IntoResponse,
    routing::get,
    Json, Router,
};
use serde::Deserialize;
use serde_json::json;
use tower_http::limit::RequestBodyLimitLayer;

use crate::schema::{
    is_author_allowed, is_valid_film_id, validate_shape, verify_signature,
    MAX_PACK_BYTES, PackError,
};
use crate::storage::Repo;

#[derive(Clone)]
pub struct AppState {
    pub repo:  Arc<Repo>,
    pub allow: Arc<HashSet<String>>,
}

pub fn router(state: AppState) -> Router {
    Router::new()
        .route("/v1/edits", get(list_edits).post(post_edit))
        // Hard upper bound on inbound body size enforced at the
        // framework level — even malformed requests can't exhaust
        // server memory before our own MAX_PACK_BYTES check fires.
        .layer(RequestBodyLimitLayer::new(MAX_PACK_BYTES + 4096))
        .with_state(state)
}

#[derive(Deserialize)]
struct EditsQuery {
    id: Option<String>,
}

async fn list_edits(
    State(state): State<AppState>,
    Query(q): Query<EditsQuery>,
) -> impl IntoResponse {
    let id = match q.id {
        Some(v) => v,
        None    => return error(StatusCode::BAD_REQUEST, "missing id query parameter"),
    };
    if !is_valid_film_id(&id) {
        return error(StatusCode::BAD_REQUEST,
                     "id must be 1..=32 ascii alphanumeric / `-` / `_`");
    }
    let packs = match state.repo.list_for_film_id(&id) {
        Ok(v)  => v,
        Err(e) => return error(StatusCode::INTERNAL_SERVER_ERROR,
                                &format!("storage: {e}")),
    };
    (StatusCode::OK, Json(json!({"packs": packs}))).into_response()
}

async fn post_edit(
    State(state): State<AppState>,
    body: String,
) -> impl IntoResponse {
    if body.len() > MAX_PACK_BYTES {
        return error(StatusCode::PAYLOAD_TOO_LARGE,
                     &format!("body exceeds {} bytes", MAX_PACK_BYTES));
    }
    let pack = match validate_shape(&body) {
        Ok(p)  => p,
        Err(e) => return error_for_shape(e),
    };
    if !is_author_allowed(&pack, &state.allow) {
        return error(StatusCode::FORBIDDEN, "author key not in allowlist");
    }
    if let Err(e) = verify_signature(&pack) {
        return error(StatusCode::UNAUTHORIZED, &format!("signature: {e}"));
    }
    let path = match state.repo.put(&pack) {
        Ok(p)  => p,
        Err(e) => return error(StatusCode::INTERNAL_SERVER_ERROR,
                                &format!("storage: {e}")),
    };
    (StatusCode::OK,
     Json(json!({"stored": path.display().to_string()})))
        .into_response()
}

fn error(code: StatusCode, msg: &str) -> axum::response::Response {
    (code, Json(json!({"error": msg}))).into_response()
}

fn error_for_shape(e: PackError) -> axum::response::Response {
    let code = match e {
        PackError::TooLarge(_) => StatusCode::PAYLOAD_TOO_LARGE,
        _                      => StatusCode::BAD_REQUEST,
    };
    error(code, &format!("schema: {e}"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::schema::{
        canonical_signed_bytes, EditPack, EmbeddedFingerprint, PackCut,
        FP_TYPE_KEYFRAMES,
    };
    use axum::body::{to_bytes, Body};
    use axum::http::Request;
    use chrono::TimeZone;
    use ed25519_dalek::{Signer, SigningKey};
    use rand::rngs::OsRng;
    use tempfile::tempdir;
    use tower::ServiceExt;

    async fn body_string(body: Body) -> String {
        let bytes = to_bytes(body, 1024 * 1024).await.unwrap();
        String::from_utf8(bytes.to_vec()).unwrap()
    }

    fn fresh_signed_pack(film_id: &str) -> (EditPack, SigningKey) {
        let sk = SigningKey::generate(&mut OsRng);
        let mut p = EditPack {
            schema:        1,
            film_id:       film_id.to_string(),
            fingerprint:   EmbeddedFingerprint {
                version: 1,
                fp_type: FP_TYPE_KEYFRAMES.to_string(),
                duration_ms: 60_000,
                approx_duration_min: 1,
                keyframe_times_ms: (0..60).map(|i| i as i64 * 1000).collect(),
                peaks: vec![],
                gaps_ms: vec![],
                inner_span_ms: 0,
            },
            author_pubkey: hex::encode(sk.verifying_key().to_bytes()),
            created_utc:   chrono::Utc.with_ymd_and_hms(2026, 5, 7, 0, 0, 0).unwrap(),
            cuts:          vec![PackCut {
                start_ms: 1_000, end_ms: 2_000,
                category: "Cruelty".to_string(),
                kind: None, score: Some(0.8), reason: None,
            }],
            comment:       None,
            sig:           None,
        };
        use base64::Engine;
        let bytes = canonical_signed_bytes(&p).unwrap();
        let sig = sk.sign(&bytes);
        p.sig = Some(base64::engine::general_purpose::STANDARD.encode(sig.to_bytes()));
        (p, sk)
    }

    fn fresh_state(allow: HashSet<String>) -> (AppState, tempfile::TempDir) {
        let dir = tempdir().unwrap();
        let repo = Repo::open(dir.path()).unwrap();
        let state = AppState { repo: Arc::new(repo), allow: Arc::new(allow) };
        (state, dir)
    }

    #[tokio::test]
    async fn get_returns_400_when_id_missing() {
        let (state, _dir) = fresh_state(HashSet::new());
        let app = router(state);
        let resp = app.oneshot(
            Request::builder().uri("/v1/edits").body(Body::empty()).unwrap()
        ).await.unwrap();
        assert_eq!(resp.status(), StatusCode::BAD_REQUEST);
    }

    #[tokio::test]
    async fn get_returns_400_when_id_malformed() {
        let (state, _dir) = fresh_state(HashSet::new());
        let app = router(state);
        let resp = app.oneshot(
            Request::builder().uri("/v1/edits?id=../etc").body(Body::empty()).unwrap()
        ).await.unwrap();
        assert_eq!(resp.status(), StatusCode::BAD_REQUEST);
    }

    #[tokio::test]
    async fn get_returns_empty_packs_for_unknown_id() {
        let (state, _dir) = fresh_state(HashSet::new());
        let app = router(state);
        let resp = app.oneshot(
            Request::builder().uri("/v1/edits?id=87")
                .body(Body::empty()).unwrap()
        ).await.unwrap();
        assert_eq!(resp.status(), StatusCode::OK);
        let body = body_string(resp.into_body()).await;
        let v: serde_json::Value = serde_json::from_str(&body).unwrap();
        assert!(v["packs"].as_array().unwrap().is_empty());
    }

    #[tokio::test]
    async fn post_then_get_round_trip() {
        let (state, _dir) = fresh_state(HashSet::new());
        let film = "87";
        let (pack, _) = fresh_signed_pack(film);
        let raw = serde_json::to_string(&pack).unwrap();

        let app = router(state.clone());
        let resp = app.clone().oneshot(
            Request::builder().method("POST").uri("/v1/edits")
                .header("content-type", "application/json")
                .body(Body::from(raw)).unwrap()
        ).await.unwrap();
        assert_eq!(resp.status(), StatusCode::OK);

        let resp2 = app.oneshot(
            Request::builder().uri(format!("/v1/edits?id={}", film))
                .body(Body::empty()).unwrap()
        ).await.unwrap();
        assert_eq!(resp2.status(), StatusCode::OK);
        let body = body_string(resp2.into_body()).await;
        let v: serde_json::Value = serde_json::from_str(&body).unwrap();
        assert_eq!(v["packs"].as_array().unwrap().len(), 1);
        assert_eq!(v["packs"][0]["film_id"], film);
    }

    #[tokio::test]
    async fn post_rejects_unsigned_pack_with_401() {
        let (state, _dir) = fresh_state(HashSet::new());
        let (mut pack, _) = fresh_signed_pack("87");
        pack.sig = None;
        let raw = serde_json::to_string(&pack).unwrap();
        let app = router(state);
        let resp = app.oneshot(
            Request::builder().method("POST").uri("/v1/edits")
                .body(Body::from(raw)).unwrap()
        ).await.unwrap();
        assert_eq!(resp.status(), StatusCode::UNAUTHORIZED);
    }

    #[tokio::test]
    async fn post_rejects_tampered_pack_with_401() {
        let (state, _dir) = fresh_state(HashSet::new());
        let (mut pack, _) = fresh_signed_pack("87");
        pack.cuts[0].score = Some(0.99);
        let raw = serde_json::to_string(&pack).unwrap();
        let app = router(state);
        let resp = app.oneshot(
            Request::builder().method("POST").uri("/v1/edits")
                .body(Body::from(raw)).unwrap()
        ).await.unwrap();
        assert_eq!(resp.status(), StatusCode::UNAUTHORIZED);
    }

    #[tokio::test]
    async fn post_rejects_unallowed_author_with_403() {
        let mut allow = HashSet::new();
        allow.insert("ff".repeat(32));
        let (state, _dir) = fresh_state(allow);
        let (pack, _) = fresh_signed_pack("87");
        let raw = serde_json::to_string(&pack).unwrap();
        let app = router(state);
        let resp = app.oneshot(
            Request::builder().method("POST").uri("/v1/edits")
                .body(Body::from(raw)).unwrap()
        ).await.unwrap();
        assert_eq!(resp.status(), StatusCode::FORBIDDEN);
    }

    #[tokio::test]
    async fn post_rejects_malformed_with_400() {
        let (state, _dir) = fresh_state(HashSet::new());
        let app = router(state);
        let resp = app.oneshot(
            Request::builder().method("POST").uri("/v1/edits")
                .body(Body::from("not json at all")).unwrap()
        ).await.unwrap();
        assert_eq!(resp.status(), StatusCode::BAD_REQUEST);
    }
}
