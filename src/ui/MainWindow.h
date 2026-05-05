#pragma once

#include <QMainWindow>
#include <QString>
#include <memory>

class QListView;
class QLabel;
class QPushButton;
class QSlider;

namespace censorcut {

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
    QString currentMoviePath() const { return m_currentMoviePath; }
    void    updateStatusBar();

    /// Returns true if the current marker state in the UI differs from
    /// what's on disk in the sidecar (or there's no sidecar yet but the
    /// user has placed markers). Reads the sidecar fresh each time.
    bool    hasUnsavedChanges() const;

    // Core
    std::unique_ptr<PlaybackController> m_playback;
    MarkerModel*  m_markers      = nullptr;

    // UI
    VideoSurface*    m_video       = nullptr;
    TimelineWidget*  m_timeline    = nullptr;
    QListView*       m_markerList  = nullptr;
    QLabel*          m_timeLabel   = nullptr;
    QPushButton*     m_playButton  = nullptr;

    // State
    QString m_currentMoviePath;
    qint64  m_pendingCutStartMs = -1;  // -1 = no pending start
    bool    m_dirty = false;
};

} // namespace censorcut
