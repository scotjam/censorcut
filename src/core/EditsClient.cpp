#include "EditsClient.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QUrlQuery>

#include <nlohmann/json.hpp>

namespace censorcut {

namespace {

constexpr qint64 kMaxResponseBytes = 32 * 1024 * 1024;  // 32 MB hard cap
constexpr int    kRequestTimeoutMs = 15000;

EditPack packFromJson(const nlohmann::json& j, const QByteArray& rawSlice)
{
    EditPack pack;
    pack.schema       = j.value("schema", 0);
    pack.filmFp       = QString::fromStdString(j.value("film_fp", std::string{}));
    pack.authorPubkey = QString::fromStdString(j.value("author_pubkey", std::string{}));
    pack.comment      = QString::fromStdString(j.value("comment", std::string{}));
    pack.rawJson      = rawSlice;
    if (j.contains("film_anchors") && j.at("film_anchors").is_array()) {
        for (const auto& aj : j.at("film_anchors")) {
            if (!aj.is_object()) continue;
            FingerprintAnchor a;
            a.tau   = aj.value("tau", 0.0);
            a.phash = QString::fromStdString(aj.value("phash", std::string{}));
            if (!a.phash.isEmpty()) pack.filmAnchors.append(a);
        }
    }
    if (j.contains("cuts") && j.at("cuts").is_array()) {
        for (const auto& cj : j.at("cuts")) {
            if (!cj.is_object()) continue;
            EditPackCut c;
            c.startMs  = cj.value("start_ms", qint64{0});
            c.endMs    = cj.value("end_ms",   qint64{0});
            c.category = QString::fromStdString(cj.value("category", std::string{}));
            c.kind     = QString::fromStdString(cj.value("kind",     std::string{}));
            c.score    = cj.value("score",  -1.0);
            c.reason   = QString::fromStdString(cj.value("reason",   std::string{}));
            if (c.endMs > c.startMs && !c.category.isEmpty())
                pack.cuts.append(c);
        }
    }
    return pack;
}

} // namespace

EditsClient::EditsClient(QObject* parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this))
{}

EditsClient::~EditsClient() = default;

QString EditsClient::configuredServerUrl()
{
    QSettings s;
    return s.value(QStringLiteral("edits/serverUrl")).toString();
}

void EditsClient::setConfiguredServerUrl(const QString& url)
{
    QSettings s;
    s.setValue(QStringLiteral("edits/serverUrl"), url);
}

void EditsClient::fetch(const QUrl& serverUrl, const QString& filmFp)
{
    if (m_reply) {
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    m_filmFp = filmFp;

    QUrl url = serverUrl;
    if (url.path().isEmpty() || url.path() == QStringLiteral("/")) {
        url.setPath(QStringLiteral("/v1/edits"));
    } else if (!url.path().endsWith(QStringLiteral("/v1/edits"))) {
        url.setPath(url.path() + QStringLiteral("/v1/edits"));
    }
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("fp"), filmFp);
    url.setQuery(q);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("censorcut/0.1"));
    req.setTransferTimeout(kRequestTimeoutMs);

    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::finished,
            this, &EditsClient::onReplyFinished);
}

void EditsClient::onReplyFinished()
{
    if (!m_reply) return;
    QNetworkReply* reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        emit fetchFailed(QStringLiteral("Network error: %1").arg(reply->errorString()));
        return;
    }
    const QByteArray body = reply->readAll();
    if (body.size() > kMaxResponseBytes) {
        emit fetchFailed(QStringLiteral("Response too large (%1 bytes)").arg(body.size()));
        return;
    }
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(body.constData(),
                                  body.constData() + body.size());
    } catch (const std::exception& e) {
        emit fetchFailed(QStringLiteral("Bad JSON: %1").arg(QString::fromUtf8(e.what())));
        return;
    }

    QList<EditPack> packs;
    if (j.contains("packs") && j.at("packs").is_array()) {
        for (const auto& pj : j.at("packs")) {
            if (!pj.is_object()) continue;
            // We don't have the per-pack raw bytes from the server's
            // {"packs":[...]} response — for now, hand each parsed pack
            // back without rawJson; the editor doesn't re-verify
            // signatures (that's the server's job, and the user has
            // chosen to trust this server when they configured the URL).
            packs.append(packFromJson(pj, QByteArray{}));
        }
    }
    emit packsFetched(m_filmFp, packs);
}

} // namespace censorcut
