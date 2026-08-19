#pragma once

#include "core/AnalysisResult.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QString>
#include <memory>

class QListView;
class QLabel;
class QCheckBox;
class QPushButton;
class QSlider;

namespace censorcut {

class AnalyzerPanel;
class ExportQueue;
class ExportQueuePanel;
class FeedbackStore;
class MarkerModel;
class PlaybackController;
class SyncProcess;
class TrustLedger;
class TimelineWidget;
class VideoSurface;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onOpenFile();
    void onSaveSidecar();
    void onExportProject();
    void onCreateShortcut();
    void onForgetFeedback();
    void onSetEditsServerUrl();
    void onPullEditsFromServer();
    void onFingerprintAvailable(const FilmFingerprint& fp);
    void onSharingSettings();
    void onReviewProposedCategories();
    void onFeedbackSharingChanged(bool on);
    void onMarkStart();
    void onMarkEnd();
    void onPositionChanged(qint64 ms);
    void onDurationKnown(qint64 ms);
    void onPlayingStateChanged(bool playing);
    void onTimelineScrubbed(qint64 ms);
    void onMarkerListContextMenu(const QPoint& pos);

private:
    void buildUi();
    void buildMenus();
    void connectSignals();
    void loadProjectFor(const QString& moviePath);
    /// On first launch (or whenever the disclaimer text version is bumped),
    /// show the disclaimer modally. Acceptance is saved in QSettings;
    /// "Quit" exits the application before the user can do anything.
    void maybeShowDisclaimer();
    void applySharingState();
    QString currentMoviePath() const { return m_currentMoviePath; }
    void    updateStatusBar();

    /// Returns true if the current marker state in the UI differs from
    /// what's on disk in the sidecar (or there's no sidecar yet but the
    /// user has placed markers). Reads the sidecar fresh each time.
    bool    hasUnsavedChanges() const;

    /// Offer to save pending marker edits before they are thrown away.
    /// Returns false if the user chose Cancel, meaning the caller should
    /// abandon whatever it was about to do. Shared by the close path and
    /// the open-another-movie path.
    bool    confirmDiscardUnsavedMarkers();

    /// Return the window to its no-movie-loaded state: stops the player,
    /// empties the marker model, and clears every widget that still shows
    /// the outgoing film. Called before a new movie is attached so nothing
    /// from the previous one can survive the switch.
    void    clearCurrentMovie();

    // Core
    std::unique_ptr<PlaybackController> m_playback;
    MarkerModel*  m_markers      = nullptr;
    ExportQueue*  m_exportQueue  = nullptr;
    FeedbackStore* m_feedback    = nullptr;
    SyncProcess*   m_sync        = nullptr;
    TrustLedger*   m_trust       = nullptr;

    // UI
    VideoSurface*    m_video       = nullptr;
    TimelineWidget*  m_timeline    = nullptr;
    QListView*       m_markerList  = nullptr;
    QLabel*          m_timeLabel   = nullptr;
    QLabel*          m_fingerprintLabel = nullptr;
    QPushButton*     m_playButton   = nullptr;
    QCheckBox*       m_previewCheck = nullptr;
    AnalyzerPanel*   m_analyzer     = nullptr;
    ExportQueuePanel* m_exportPanel = nullptr;

    // State
    QString m_currentMoviePath;
    qint64  m_pendingCutStartMs = -1;  // -1 = no pending start
    bool    m_dirty = false;
    bool    m_previewMode = false;
    bool    m_userScrubbing = false;
    qint64  m_lastPlaybackPos = -1;  // for forward-crossing detection in preview
    /// Seek throttling for scrub drags. A drag emits a scrubbed() per mouse
    /// move; issuing a libVLC seek for every one floods the demuxer and the
    /// picture ends up well behind the cursor. We seek at most every
    /// kScrubSeekIntervalMs and flush the last requested position on release.
    QElapsedTimer m_scrubSeekThrottle;
    qint64        m_pendingScrubMs = -1;  // -1 = nothing deferred
    /// J/K/L tracks the user's *intended* rate independently of libVLC,
    /// because libvlc_get_rate can return slightly different floats on
    /// readback (and 0 when paused). Reset to 1.0 by the '1' key.
    double  m_intendedRate = 1.0;
};

} // namespace censorcut
