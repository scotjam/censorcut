#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
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

    /// True while a paused seek is still converging on its target (the muted
    /// micro-play walk). The player reports "playing" during the walk, but it
    /// is servicing a seek, not user playback — UI logic that reacts to
    /// forward playback (e.g. preview-mode cut skipping) should stand down.
    bool   isSeekSettling() const { return m_correcting; }

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
    void emitPosition(qint64 ms);

    /// Frame-exact paused seeks. A paused seek positions the demuxer on the
    /// keyframe at or before the target — up to several seconds early on
    /// long-GOP encodes — and --no-input-fast-seek does not help, because the
    /// "decode forward to the target" half of a precise seek only happens
    /// when something is pulling frames, which a paused pipeline is not.
    /// Worse, get_time() can keep echoing the requested time while the frame
    /// on screen is still that early keyframe, so the recorded mark and the
    /// displayed picture silently disagree. The correction resumes playback
    /// (muted, fast, slowing near the target) and pauses again the moment the
    /// advancing clock — which, unlike a paused read, cannot be an echo —
    /// reaches the target. next_frame stepping was tried first and measured
    /// to be a no-op on mkv input.
    void beginPausedSeekCorrection();
    void onSeekCorrectionTick();
    void finishSeekCorrection(qint64 finalMs);
    void cancelSeekCorrection(bool repause = true);

    enum class CorrectPhase { Idle, Starting, Walking };

    libvlc_instance_t*     m_vlc      = nullptr;
    libvlc_media_player_t* m_player   = nullptr;
    libvlc_media_t*        m_media    = nullptr;

    qint64 m_durationCached = 0;

    /// libvlc_MediaPlayerTimeChanged only fires about every 250 ms, which is
    /// far too coarse for frame-accurate marking — the playhead visibly lags
    /// the picture. Poll the player while it is actually playing and treat the
    /// event as a correction source. Deduplicated via m_lastEmittedPos so a
    /// paused-but-not-yet-stopped poll doesn't spam identical signals.
    QTimer m_pollTimer;
    qint64 m_lastEmittedPos = -1;

    // Paused-seek correction state (see beginPausedSeekCorrection).
    QTimer        m_correctTimer;
    QElapsedTimer m_correctWall;
    CorrectPhase  m_correctPhase   = CorrectPhase::Idle;
    qint64 m_seekTargetMs   = -1;    ///< most recent seek() request
    qint64 m_correctLastMs  = -1;    ///< last clock value the walk observed
    int    m_correctMuteWas = 0;     ///< audio mute state to restore (-1 = no aout)
    float  m_correctRateWas = 1.0f;  ///< playback rate to restore
    double m_correctRateSet = 0.0;   ///< rate the walk last requested
    bool   m_correctSawMove = false; ///< clock movement seen since resume (echo defense)
    bool   m_correctSawBelow = false; ///< walk observed a clock value below target
    bool   m_correcting     = false;
};

} // namespace censorcut
