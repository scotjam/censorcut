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

struct EditPack {
    int                  schema = 0;
    QString              filmFp;
    QList<FingerprintAnchor> filmAnchors;  // re-uses M8.2 type
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

    /// Issue GET <serverUrl>/v1/edits?fp=<filmFp>. Emits packsFetched
    /// or fetchFailed exactly once.
    void fetch(const QUrl& serverUrl, const QString& filmFp);

signals:
    void packsFetched(const QString& filmFp, const QList<EditPack>& packs);
    void fetchFailed(const QString& reason);

private slots:
    void onReplyFinished();

private:
    QNetworkAccessManager* m_nam = nullptr;
    QNetworkReply*         m_reply = nullptr;
    QString                m_filmFp;
};

} // namespace censorcut
