#pragma once

#include "AnalysisResult.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

namespace censorcut {

/// One edit pack as the server returns it. Mirrors the schema in
/// edits/src/schema.rs but in Qt types so the editor doesn't need
/// to depend on the Rust crate.
struct EditPackCut {
    qint64  startMs;
    qint64  endMs;
    QString category;
    QString kind;        // "confirmed" / "suggested" / empty
    double  score;       // -1.0 if absent
    QString reason;
};

/// A pack's embedded fingerprint, used by the receiver to decide whether
/// the pack's cuts apply to their copy of the film. Carries whichever of
/// F's keyframeTimesMs or v9's peaks the publisher's fingerprint was
/// based on. The Rust schema follows the same shape as
/// FilmFingerprint (see edits/src/schema.rs after the M9 migration).
struct EditPack {
    int                  schema = 0;
    /// Bucket key the server uses for indexing — typically the
    /// publisher's approxDurationMin. Receivers ALSO verify by running
    /// the full fingerprint match against `fingerprint` below.
    QString              filmId;
    FilmFingerprint      fingerprint;   // shape mirrors AnalysisResult.fingerprint
    QString              authorPubkey;
    QString              comment;
    QList<EditPackCut>   cuts;
    /// Raw JSON (the server's exact bytes for this pack) — kept so we
    /// can hand it to anyone who wants to verify the signature
    /// independently of the editor.
    QByteArray           rawJson;
};

/// Asynchronous HTTP client for the M8.5 edits server. One instance is
/// fine to share across the app; signals are emitted on the UI thread.
class EditsClient : public QObject {
    Q_OBJECT
public:
    explicit EditsClient(QObject* parent = nullptr);
    ~EditsClient() override;

    /// Default server URL is read from / written to QSettings under
    /// the key "edits/serverUrl". Empty string means "no server".
    static QString  configuredServerUrl();
    static void     setConfiguredServerUrl(const QString& url);

    /// Issue GET <serverUrl>/v1/edits?id=<filmId>. The filmId is the
    /// fingerprint's bucket key (typically approxDurationMin as a
    /// stringified int). Emits packsFetched or fetchFailed exactly once.
    void fetch(const QUrl& serverUrl, const QString& filmId);

signals:
    void packsFetched(const QString& filmId, const QList<EditPack>& packs);
    void fetchFailed(const QString& reason);

private slots:
    void onReplyFinished();

private:
    QNetworkAccessManager* m_nam = nullptr;
    QNetworkReply*         m_reply = nullptr;
    QString                m_filmId;
};

} // namespace censorcut
