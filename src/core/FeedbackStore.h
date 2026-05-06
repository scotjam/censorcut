#pragma once

#include "AnalysisResult.h"
#include "Marker.h"

#include <QList>
#include <QObject>
#include <QString>

namespace censorcut {

/// Persists user accept/reject decisions on Source::Suggested markers as
/// JSONL records under ~/.censorcut/feedback.jsonl. Each record stores the
/// CLIP embedding for one frame inside the marker's range plus the
/// category, peak score, prompts that fired, and the decision. No source
/// path, no movie title, no exact timestamps — just the semantic vector
/// and the decision.
///
/// The next analysis run reads these to penalize candidate frames whose
/// embeddings are close to past rejections (and boost ones close to past
/// accepts).
class FeedbackStore : public QObject {
    Q_OBJECT
public:
    explicit FeedbackStore(QObject* parent = nullptr);

    enum class Decision { Accepted, Rejected };

    /// Default file location: ~/.censorcut/feedback.jsonl.
    static QString defaultPath();

    /// Override the storage path (used by tests).
    void setStoragePath(const QString& path);
    QString storagePath() const;

    /// Snapshot of frame embeddings from the most recent analysis. Held by
    /// reference; the panel pushes it on each completed run so any
    /// subsequent decision can look up the embeddings inside a marker's
    /// time range.
    void setLatestEmbeddings(const QList<FrameEmbedding>& embeddings);
    const QList<FrameEmbedding>& latestEmbeddings() const;

    /// Append one JSONL row per frame inside the marker's [startMs, endMs).
    /// Returns the number of rows written; 0 means there were no embeddings
    /// for that range or I/O failed.
    int recordDecision(const Marker& marker, Decision decision);

    /// Wipe the local feedback file. Used by Settings → Forget feedback.
    bool forgetAll();

    /// Total rows currently stored (cheap line-count).
    int storedRowCount() const;

private:
    QString             m_path;
    QList<FrameEmbedding> m_embeddings;
};

} // namespace censorcut
