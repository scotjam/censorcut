#pragma once

#include <QObject>
#include <QString>
#include <qwindowdefs.h>

// libVLC opaque types — keep <vlc/vlc.h> out of the header to limit blast radius.
struct libvlc_instance_t;
struct libvlc_media_player_t;
struct libvlc_media_t;
struct libvlc_event_t;

namespace censorcut {

/// Thin wrapper over libVLC. Holds one instance + one player.
/// All public methods are safe to call from the GUI thread.
/// libVLC fires events on its own thread; we marshal them to the GUI thread
/// via Qt::QueuedConnection (handled internally — listeners just connect normally).
class PlaybackController : public QObject {
    Q_OBJECT
public:
    explicit PlaybackController(QObject* parent = nullptr);
    ~PlaybackController() override;

    PlaybackController(const PlaybackController&) = delete;
    PlaybackController& operator=(const PlaybackController&) = delete;

    /// Embed the video output into a native window handle owned by a QWidget.
    /// Pass the result of QWidget::winId() (cast appropriately per platform).
    void setVideoSink(WId winId);

    /// Open a media file. Returns true if the media was created (playback may
    /// still fail later — listen for errorOccurred).
    bool open(const QString& path);

    void play();
    void pause();
    void togglePlayPause();
    void stop();

    void seek(qint64 ms);
    void seekRelative(qint64 deltaMs);
    void stepFrame(int direction);  // +1 forward, -1 backward (approximate)
    void setRate(double rate);

    qint64 position() const;     // ms
    qint64 duration() const;     // ms; 0 if unknown
    double rate() const;
    bool   isPlaying() const;

signals:
    void mediaOpened(const QString& path);
    void durationKnown(qint64 ms);
    void positionChanged(qint64 ms);
    void playingStateChanged(bool playing);
    void endReached();
    void errorOccurred(const QString& message);

private:
    static void onVlcEvent(const libvlc_event_t* ev, void* opaque);
    void attachEvents();
    void detachEvents();

    libvlc_instance_t*     m_vlc      = nullptr;
    libvlc_media_player_t* m_player   = nullptr;
    libvlc_media_t*        m_media    = nullptr;

    qint64 m_durationCached = 0;
};

} // namespace censorcut
