#pragma once

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
    void onForgetFeedback();
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
    QString currentMoviePath() const { return m_currentMoviePath; }
    void    updateStatusBar();

    /// Returns true if the current marker state in the UI differs from
    /// what's on disk in the sidecar (or there's no sidecar yet but the
    /// user has placed markers). Reads the sidecar fresh each time.
    bool    hasUnsavedChanges() const;

    // Core
    std::unique_ptr<PlaybackController> m_playback;
    MarkerModel*  m_markers      = nullptr;
    ExportQueue*  m_exportQueue  = nullptr;
    FeedbackStore* m_feedback    = nullptr;

    // UI
    VideoSurface*    m_video       = nullptr;
    TimelineWidget*  m_timeline    = nullptr;
    QListView*       m_markerList  = nullptr;
    QLabel*          m_timeLabel   = nullptr;
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
    /// J/K/L tracks the user's *intended* rate independently of libVLC,
    /// because libvlc_get_rate can return slightly different floats on
    /// readback (and 0 when paused). Reset to 1.0 by the '1' key.
    double  m_intendedRate = 1.0;
};

} // namespace censorcut
