#include "FeedbackStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>

#include <nlohmann/json.hpp>

namespace censorcut {

FeedbackStore::FeedbackStore(QObject* parent)
    : QObject(parent)
{
    m_path = defaultPath();
}

QString FeedbackStore::defaultPath()
{
    // ~/.censorcut/feedback.jsonl on every platform — keeping it under
    // QStandardPaths::HomeLocation matches the user's stated mental model
    // ("user data lives in the home directory") rather than a Roaming
    // AppData hideout.
    const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    return QDir::cleanPath(home + QStringLiteral("/.censorcut/feedback.jsonl"));
}

void FeedbackStore::setStoragePath(const QString& path)
{
    m_path = path;
}

QString FeedbackStore::storagePath() const
{
    return m_path;
}

void FeedbackStore::setLatestEmbeddings(const QList<FrameEmbedding>& embeddings)
{
    m_embeddings = embeddings;
}

const QList<FrameEmbedding>& FeedbackStore::latestEmbeddings() const
{
    return m_embeddings;
}

int FeedbackStore::recordDecision(const Marker& marker, Decision decision)
{
    if (m_embeddings.isEmpty()) return 0;

    QFileInfo info(m_path);
    if (!info.dir().exists() && !QDir().mkpath(info.absolutePath())) return 0;

    QFile f(m_path);
    if (!f.open(QIODevice::Append | QIODevice::Text)) return 0;
    QTextStream out(&f);

    int written = 0;
    const char* decisionStr = (decision == Decision::Accepted) ? "accept" : "reject";
    for (const FrameEmbedding& emb : m_embeddings) {
        if (emb.timeMs < marker.startMs || emb.timeMs >= marker.endMs) continue;
        if (emb.vec.isEmpty()) continue;

        nlohmann::json row;
        row["schema"]    = 1;
        row["category"]  = marker.category.toStdString();
        row["decision"]  = decisionStr;
        row["score"]     = marker.confidence;
        nlohmann::json vec = nlohmann::json::array();
        vec.get_ptr<nlohmann::json::array_t*>()->reserve(emb.vec.size());
        for (float v : emb.vec) vec.push_back(double(v));
        row["vec"] = std::move(vec);

        out << QString::fromStdString(row.dump()) << QStringLiteral("\n");
        ++written;
    }
    return written;
}

bool FeedbackStore::forgetAll()
{
    QFile f(m_path);
    if (!f.exists()) return true;
    return f.remove();
}

int FeedbackStore::storedRowCount() const
{
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return 0;
    int n = 0;
    while (!f.atEnd()) { f.readLine(); ++n; }
    return n;
}

} // namespace censorcut
