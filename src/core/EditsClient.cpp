#include "EditsClient.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QUrlQuery>

#include <nlohmann/json.hpp>

namespace censorcut {

namespace {

constexpr qint64 kMaxResponseBytes      = 32 * 1024 * 1024;  // 32 MB hard cap
constexpr int    kRequestTimeoutMs      = 15000;
/// Match server-side caps in edits/src/schema.rs.
constexpr int    kMaxKeyframesPerPack   = 20000;
constexpr int    kMaxPeaksPerPack       = 200;
constexpr int    kMaxCutsPerPack        = 500;

void parseEmbeddedFingerprint(const nlohmann::json& fj, FilmFingerprint& fp)
{
    fp.version           = fj.value("version", 1);
    fp.type              = QString::fromStdString(fj.value("type", std::string{}));
    fp.durationMs        = fj.value("durationMs", qint64{0});
    fp.approxDurationMin = fj.value("approxDurationMin", 0);
    if (fp.type == QLatin1String(fp_type::Keyframes)) {
        if (fj.contains("keyframeTimesMs") && fj.at("keyframeTimesMs").is_array()) {
            const auto& arr = fj.at("keyframeTimesMs");
            fp.keyframeTimesMs.reserve(static_cast<int>(arr.size()));
            for (const auto& v : arr) {
                if (fp.keyframeTimesMs.size() >= kMaxKeyframesPerPack) break;
                if (v.is_number_integer() || v.is_number_unsigned())
                    fp.keyframeTimesMs.append(v.get<qint64>());
            }
        }
    } else if (fp.type == QLatin1String(fp_type::AudioPeakGaps)) {
        fp.innerSpanMs = fj.value("innerSpanMs", qint64{0});
        if (fj.contains("peaks") && fj.at("peaks").is_array()) {
            for (const auto& pj : fj.at("peaks")) {
                if (fp.peaks.size() >= kMaxPeaksPerPack) break;
                if (!pj.is_object()) continue;
                FingerprintPeak p;
                p.tMs   = pj.value("tMs", qint64{0});
                p.phash = QString::fromStdString(pj.value("phash", std::string{}));
                fp.peaks.append(p);
            }
        }
        if (fj.contains("gapsMs") && fj.at("gapsMs").is_array()) {
            const auto& arr = fj.at("gapsMs");
            fp.gapsMs.reserve(static_cast<int>(arr.size()));
            for (const auto& v : arr) {
                if (fp.gapsMs.size() >= kMaxPeaksPerPack) break;
                if (v.is_number_integer() || v.is_number_unsigned())
                    fp.gapsMs.append(v.get<qint64>());
            }
        }
    }
}

EditPack packFromJson(const nlohmann::json& j, const QByteArray& rawSlice)
{
    EditPack pack;
    pack.schema       = j.value("schema", 0);
    pack.filmId       = QString::fromStdString(j.value("film_id", std::string{}));
    pack.authorPubkey = QString::fromStdString(j.value("author_pubkey", std::string{}));
    pack.comment      = QString::fromStdString(j.value("comment", std::string{}));
    pack.rawJson      = rawSlice;
    if (j.contains("fingerprint") && j.at("fingerprint").is_object()) {
        parseEmbeddedFingerprint(j.at("fingerprint"), pack.fingerprint);
    }
    if (j.contains("cuts") && j.at("cuts").is_array()) {
        for (const auto& cj : j.at("cuts")) {
            if (pack.cuts.size() >= kMaxCutsPerPack) break;
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

void EditsClient::fetch(const QUrl& serverUrl, const QString& filmId)
{
    if (m_reply) {
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    m_filmId = filmId;

    QUrl url = serverUrl;
    if (url.path().isEmpty() || url.path() == QStringLiteral("/")) {
        url.setPath(QStringLiteral("/v1/edits"));
    } else if (!url.path().endsWith(QStringLiteral("/v1/edits"))) {
        url.setPath(url.path() + QStringLiteral("/v1/edits"));
    }
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("id"), filmId);
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
    emit packsFetched(m_filmId, packs);
}

} // namespace censorcut
