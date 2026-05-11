#pragma once

#include "core/AgeProfile.h"
#include "core/AnalysisResult.h"

#include <QString>
#include <QUuid>
#include <QWidget>

class QFrame;
class QLabel;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;

namespace censorcut {

class AnalysisController;
class FeedbackStore;
class MarkerModel;
class PlaybackController;
class TrustLedger;

/// The right-pane analyzer UI: age selector, profile label, Run button,
/// progress, a result summary, and a "review pending suggestions" walker
/// that lets the user step through Suggested markers, play each one, and
/// click Confirm/Reject. Owns an AnalysisController; pushes
/// Source::Suggested markers into the supplied MarkerModel.
class AnalyzerPanel : public QWidget {
    Q_OBJECT
public:
    AnalyzerPanel(MarkerModel* markers,
                  PlaybackController* playback,
                  FeedbackStore* feedback,
                  QWidget* parent = nullptr);

    /// MainWindow injects its TrustLedger so the review actions can
    /// drive reward/penalty for each Suggested marker's contributing
    /// authors. Can be null in tests.
    void setTrustLedger(TrustLedger* ledger);

    /// Set the current source video and its known duration. Pass an empty
    /// path to disable Run.
    void setMovie(const QString& sourcePath, qint64 durationMs);

    /// Kick off a fast fingerprint-only analysis (skips CLIP / Whisper /
    /// loudness — just the scene-cut + pHash video fingerprint). Used to
    /// identify a movie immediately on open. The resulting fingerprint
    /// becomes available via latestFingerprint().
    bool runFingerprintOnly();

    /// The most recent analysis result's fingerprint (empty if no run
    /// yet). Used by the Pull-edits-server flow as the lookup key.
    FilmFingerprint latestFingerprint() const { return m_latestFingerprint; }

    /// The age the user has selected. Other code can use AgeProfile::forAge
    /// to translate to a profile.
    int  selectedAge() const;
    void setSelectedAge(int age);

signals:
    void ageChanged(int age);
    /// Fires after every analysis run (full or fingerprint-only) once
    /// the result is available — listeners can also read latestFingerprint().
    /// The argument carries the full fingerprint (type, durationMs,
    /// keyframeTimesMs / peaks); the status bar uses the type + bucket
    /// key for its display.
    void fingerprintAvailable(const FilmFingerprint& fp);

private slots:
    void onRunClicked();
    void onCancelClicked();
    void onProgress(double fraction);
    void onPhase(const QString& phase);
    void onCompleted(const AnalysisResult& result);
    void onFailed(const QString& reason);

    void onPositionChanged(qint64 ms);
    void onMarkersChanged();

    void onReviewPrev();
    void onReviewNext();
    void onReviewConfirm();
    void onReviewReject();

private:
    void setRunning(bool running);
    void describeProfile();
    void refreshReviewUi();
    void startReviewFor(const QUuid& id, qint64 newPositionMs = -1);
    /// Find the next/previous Pending marker relative to a reference time.
    QUuid pendingAfter(qint64 referenceMs)  const;
    QUuid pendingBefore(qint64 referenceMs) const;
    void  setStatusAndAdvance(int newStatus);

    MarkerModel*        m_markers    = nullptr;
    PlaybackController* m_playback   = nullptr;
    FeedbackStore*      m_feedback   = nullptr;
    TrustLedger*        m_trust      = nullptr;
    AnalysisController* m_controller = nullptr;

    QString m_sourcePath;
    qint64  m_durationMs = 0;
    int     m_age = 8;
    FilmFingerprint  m_latestFingerprint;

    // Review state
    QUuid  m_reviewId;          // marker currently being auditioned, null if none
    qint64 m_reviewPauseAtMs = -1;  // playhead position at which to auto-pause
    qint64 m_reviewPreRollMs  = 800;
    qint64 m_reviewPostRollMs = 600;

    QSpinBox*    m_ageSpin       = nullptr;
    QLabel*      m_profileLabel  = nullptr;
    QPushButton* m_runBtn        = nullptr;
    QPushButton* m_cancelBtn     = nullptr;
    QProgressBar* m_progress     = nullptr;
    QLabel*      m_phaseLabel    = nullptr;
    QLabel*      m_summaryLabel  = nullptr;
    QSlider*     m_sensitivitySlider = nullptr;
    QLabel*      m_sensitivityLabel  = nullptr;

    // Review section
    QFrame*      m_reviewFrame   = nullptr;
    QLabel*      m_reviewCount   = nullptr;
    QLabel*      m_reviewCurrent = nullptr;
    QPushButton* m_reviewPrev    = nullptr;
    QPushButton* m_reviewReject  = nullptr;
    QPushButton* m_reviewConfirm = nullptr;
    QPushButton* m_reviewSkip    = nullptr;
    QPushButton* m_reviewNext    = nullptr;
};

} // namespace censorcut
