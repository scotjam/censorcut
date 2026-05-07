#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

namespace censorcut {

/// Per-installation reputation ledger keyed on author pubkey (hex).
/// Trust is a local, never-shared score that says "how predictive has
/// this peer's feedback been of suggestions I end up accepting?".
///
/// Scoring rules:
///   - Unseen pubkey starts at the floor (0.1) modulated upward by
///     transitive endorsement bootstrap (see `bootstrapFloorFor`).
///   - User accepts a suggestion influenced by author X → X's score
///     ticks up by `kAcceptDelta`.
///   - User rejects a suggestion influenced by author X → X's score
///     ticks down by `kRejectDelta`.
///   - Score clamped to [0, 2.0].
///
/// The ledger is persisted at `~/.censorcut/trust.json`. The file
/// format is a JSON object with two top-level keys:
///   - `direct`: { "<pubkey-hex>": { "score": <float>, "n": <int> } }
///   - `endorsements`: list of `{author: <pubkey>, target: <pubkey>,
///     score: <float>}` rows pulled from peers (used for transitive
///     trust). Capped at `kMaxStoredEndorsements` total rows; LRU-evicted.
class TrustLedger : public QObject {
    Q_OBJECT
public:
    explicit TrustLedger(QObject* parent = nullptr);

    /// Returns the current weight to apply to a row signed by `pubkey`.
    /// For seen pubkeys, this is the direct trust score. For unseen
    /// pubkeys, this is the transitive bootstrap floor (max 0.4).
    double weightFor(const QString& pubkey) const;

    /// Tick `pubkey`'s direct trust up. Records that we have direct
    /// experience with this peer.
    void rewardAuthor(const QString& pubkey);

    /// Tick `pubkey`'s direct trust down.
    void penalizeAuthor(const QString& pubkey);

    /// Replace an entire peer's published endorsement list. Used when
    /// the sync sidecar delivers a fresh `endorsements.jsonl` row from
    /// `author`. Unknown authors are ignored (we only act on endorsement
    /// data from peers whose own published score we know directly or
    /// transitively above the publish threshold of 0.8).
    void setEndorsementsFrom(const QString& author,
                             const QHash<QString, double>& targets);

    /// Top entries (score >= 0.8) for our own outbound endorsement
    /// publishing. Capped to 50 entries by score, descending.
    QHash<QString, double> outboundEndorsements() const;

    /// Wipe everything. Used by the disclaimer's "Forget feedback"
    /// path so trust state doesn't outlive the user's intent to forget.
    void reset();

    void load();
    void save() const;

    static constexpr double kFloor          = 0.1;
    static constexpr double kCap            = 2.0;
    static constexpr double kAcceptDelta    = 0.05;
    static constexpr double kRejectDelta    = 0.08;
    static constexpr double kPublishCutoff  = 0.8;
    static constexpr double kBootstrapCap   = 0.4;
    static constexpr double kHopDamping     = 0.5;
    static constexpr int    kMaxDepth       = 4;
    static constexpr int    kMaxOutbound    = 50;
    static constexpr int    kMaxStoredEndorsements = 5000;

signals:
    void changed();

private:
    struct Direct {
        double score = kFloor;
        int    interactions = 0;
    };
    QHash<QString, Direct> m_direct;

    /// Per-author endorsement list. Author → (target → published score).
    QHash<QString, QHash<QString, double>> m_endorsements;

    /// Computed bootstrap floor for an unseen pubkey, summing over
    /// damped paths up to `kMaxDepth`. Returns 0 if no chain reaches.
    double bootstrapFloorFor(const QString& target) const;

    QString filePath() const;
};

} // namespace censorcut
