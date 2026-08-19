#include "PlaybackController.h"

#include <QDebug>
#include <QFileInfo>
#include <QMetaObject>
#include <QUrl>
#include <Qt>

#include <vlc/vlc.h>

namespace censorcut {

namespace {

// libVLC events we care about
constexpr libvlc_event_e kEvents[] = {
    libvlc_MediaPlayerPlaying,
    libvlc_MediaPlayerPaused,
    libvlc_MediaPlayerStopped,
    libvlc_MediaPlayerEndReached,
    libvlc_MediaPlayerEncounteredError,
    libvlc_MediaPlayerTimeChanged,
    libvlc_MediaPlayerLengthChanged,
};

// How often to poll the player's clock while playing. ~24 Hz: fine enough
// that the playhead reads as smooth, cheap enough to be irrelevant.
constexpr int kPollIntervalMs = 40;

// How long to give the demuxer to settle before reading back a seek's true
// landing time.
constexpr int kSeekSettleMs = 150;

// Paused-seek correction (micro-play) tuning.
//
// libvlc_media_player_next_frame would be the obvious tool for walking the
// decoder forward, but it was measured to be a no-op on mkv input — the vout
// never advances, the clock never moves. So the walk resumes real playback
// instead: muted, at kCatchupRate through the bulk of the GOP, dropping to
// kApproachRate inside kApproachWindowMs of the target so the pause command's
// latency costs a few ms of media time instead of a few hundred.
constexpr int    kCorrectTickMs      = 10;
constexpr double kCatchupRate        = 3.0;    // clock still advances if the
                                               // decoder can't sustain this —
                                               // VLC drops frames, which is fine
constexpr double kApproachRate       = 0.25;
constexpr qint64 kApproachWindowMs   = 700;    // wide enough that set_rate's
                                               // own latency applies in time
constexpr int    kStartTimeoutMs     = 2000;   // play() never engaged → bail
constexpr int    kCorrectWallCapMs   = 20000;  // hard stop for a dead pipeline
constexpr int    kCorrectRestReadMs  = 200;    // read-back after the final pause

// Resume watchdog: how long play() waits for is_playing before concluding the
// input wedged (paused-seek-then-resume does this on AVI) and re-kicking it.
constexpr int    kResumeKickMs       = 800;

} // namespace

PlaybackController::PlaybackController(QObject* parent)
    : QObject(parent)
{
    m_pollTimer.setInterval(kPollIntervalMs);
    m_pollTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_pollTimer, &QTimer::timeout, this, [this]{
        if (!m_player) return;
        emitPosition(libvlc_media_player_get_time(m_player));
    });

    m_correctTimer.setInterval(kCorrectTickMs);
    m_correctTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_correctTimer, &QTimer::timeout,
            this, &PlaybackController::onSeekCorrectionTick);

    // libvlc_new reads the machine's VLC configuration, so anything the user
    // has set in their VLC preferences leaks in here. Seek behaviour has to be
    // deterministic — marker times come from libvlc_media_player_get_time()
    // after a seek (see MainWindow's `[`/`]` handling), so a sloppy seek does
    // not just look wrong, it records the wrong cut point.
    //
    //   --no-input-fast-seek  demux to the keyframe before the target, then
    //                         decode forward to the exact time. Pinned so a
    //                         user's VLC preference can't make things worse,
    //                         but measured to be insufficient on its own: the
    //                         decode-forward half only happens while frames
    //                         are being pulled, so a PAUSED seek still lands
    //                         on the keyframe (up to ~9 s early on long-GOP
    //                         H.264). beginPausedSeekCorrection() closes that
    //                         gap by walking the decoder to the target.
    //   --avi-index=1         AVIs with a missing or broken index normally
    //                         raise a "try to fix it?" dialog, which libvlc
    //                         has no provider for — so seeking silently stays
    //                         broken. 1 = always rebuild the index.
    static const char* args[] = {
        "--no-video-title-show",
        "--quiet",
        "--no-input-fast-seek",
        "--avi-index=1",
    };
    m_vlc = libvlc_new(static_cast<int>(std::size(args)), args);
    if (!m_vlc) {
        qWarning() << "libvlc_new failed";
        return;
    }
    m_player = libvlc_media_player_new(m_vlc);
    if (!m_player) {
        qWarning() << "libvlc_media_player_new failed";
        return;
    }
    attachEvents();
}

PlaybackController::~PlaybackController()
{
    m_pollTimer.stop();
    m_correctTimer.stop();
    if (m_player) {
        detachEvents();
        libvlc_media_player_stop(m_player);
        libvlc_media_player_release(m_player);
        m_player = nullptr;
    }
    if (m_media) {
        libvlc_media_release(m_media);
        m_media = nullptr;
    }
    if (m_vlc) {
        libvlc_release(m_vlc);
        m_vlc = nullptr;
    }
}

void PlaybackController::setVideoSink(WId winId)
{
    if (!m_player) return;
#if defined(Q_OS_WIN)
    libvlc_media_player_set_hwnd(m_player, reinterpret_cast<void*>(winId));
#elif defined(Q_OS_MACOS)
    libvlc_media_player_set_nsobject(m_player, reinterpret_cast<void*>(winId));
#else
    libvlc_media_player_set_xwindow(m_player, static_cast<uint32_t>(winId));
#endif
}

bool PlaybackController::open(const QString& path)
{
    if (!m_vlc || !m_player) return false;
    cancelSeekCorrection();
    ++m_playEpoch;             // disarm the resume watchdog; see pause()

    // Tear the current input down before attaching new media. set_media on a
    // running player leaves the previous film's last decoded frame on the
    // video surface until the new one produces a picture, which on a slow
    // source reads as "the old movie is still open".
    libvlc_media_player_stop(m_player);

    if (m_media) {
        libvlc_media_release(m_media);
        m_media = nullptr;
    }

    // Use libvlc_media_new_location with a file:// URL rather than
    // libvlc_media_new_path: the latter does its own locale conversion on
    // Windows which mangles UTF-8 input and refuses paths it can't round-trip.
    // QUrl::fromLocalFile handles drive letters, UNC paths, and percent
    // encoding correctly.
    const QByteArray url = QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toEncoded();
    m_media = libvlc_media_new_location(m_vlc, url.constData());
    if (!m_media) {
        emit errorOccurred(QStringLiteral("libvlc_media_new_location failed for %1").arg(path));
        return false;
    }
    libvlc_media_player_set_media(m_player, m_media);
    m_durationCached = 0;
    m_lastEmittedPos = -1;
    m_seekTargetMs   = -1;
    emit mediaOpened(path);
    return true;
}

void PlaybackController::play()
{
    // The user wants playback, so the walk hands over mid-flight: restore
    // rate and mute but do NOT re-pause — playback continues from wherever
    // the walk had decoded to, which is the honest position. Invalidating
    // the seek target also disarms any settle callback still in flight, so
    // a walk can never start against playback the user just asked for (the
    // race window is seek() → play() within kSeekSettleMs).
    cancelSeekCorrection(/*repause=*/false);
    m_seekTargetMs = -1;
    if (!m_player) return;
    libvlc_media_player_play(m_player);

    // Resume watchdog. A play issued after a paused seek can wedge — the AVI
    // input never leaves its buffering limbo and is_playing stays false
    // indefinitely (measured: 8+ s dead). Re-issuing the seek at the current
    // position kicks the input thread back to life. Healthy files engage in
    // well under the delay, so for them this never fires.
    const int epoch = ++m_playEpoch;
    QTimer::singleShot(kResumeKickMs, this, [this, epoch]{
        if (!m_player || epoch != m_playEpoch) return;   // superseded
        if (isPlaying() || m_correcting) return;
        const qint64 here = libvlc_media_player_get_time(m_player);
        if (here >= 0) libvlc_media_player_set_time(m_player, here);
        libvlc_media_player_play(m_player);
    });
}

void PlaybackController::pause()
{
    cancelSeekCorrection();   // repauses as part of the cancel
    ++m_playEpoch;            // disarm the resume watchdog: the user wants
                              // paused, so "not playing yet" is not a wedge
    if (m_player) libvlc_media_player_set_pause(m_player, 1);
}

void PlaybackController::togglePlayPause()
{
    if (!m_player) return;
    if (m_correcting) {
        // The player is "playing" only because a seek walk borrowed it. Treat
        // the toggle as a pause of the user's session: settle where the walk
        // has reached. A second toggle then plays, deterministically.
        cancelSeekCorrection();
        return;
    }
    if (libvlc_media_player_is_playing(m_player)) {
        ++m_playEpoch;         // disarm the resume watchdog; see pause()
        libvlc_media_player_set_pause(m_player, 1);
    } else {
        m_seekTargetMs = -1;   // disarm a pending settle walk; see play()
        libvlc_media_player_play(m_player);
    }
}

void PlaybackController::stop()
{
    cancelSeekCorrection();
    m_seekTargetMs = -1;
    ++m_playEpoch;             // disarm the resume watchdog; see pause()
    if (m_player) libvlc_media_player_stop(m_player);
}

void PlaybackController::seek(qint64 ms)
{
    if (!m_player) return;
    cancelSeekCorrection();
    m_seekTargetMs = ms;
    libvlc_media_player_set_time(m_player, ms);

    // Report the seek target straight away so the UI is responsive rather than
    // waiting a poll interval for the clock to catch up...
    emitPosition(ms);

    // ...then reconcile with where the player actually landed. While playing
    // the poll timer keeps the clock honest. While paused the demuxer has
    // landed on a keyframe that can sit seconds before the target, and
    // nothing will decode past it on its own — walk it forward.
    QTimer::singleShot(kSeekSettleMs, this, [this, ms]{
        if (!m_player || ms != m_seekTargetMs) return;   // superseded
        if (isPlaying()) {
            emitPosition(libvlc_media_player_get_time(m_player));
        } else {
            beginPausedSeekCorrection();
        }
    });
}

void PlaybackController::seekRelative(qint64 deltaMs)
{
    // While a paused-seek walk is still converging, position() reads a frame
    // partway through the GOP. Chain from the requested target instead so
    // repeated arrow presses accumulate exactly instead of compounding the
    // walk's transient undershoot.
    const qint64 base = m_correcting ? m_seekTargetMs : position();
    seek(base + deltaMs);
}

void PlaybackController::beginPausedSeekCorrection()
{
    // get_time() after a paused seek can simply echo the requested time while
    // the picture on screen is still the preceding keyframe — measured at up
    // to 9.4 s early on a long-GOP 720p H.264 encode. The only clock that can
    // be trusted is one that is advancing because frames are being decoded,
    // so resume playback (muted, fast) and pause again the moment the clock
    // reaches the target. The walk is visible as a brief silent fast-forward
    // on long GOPs, which is honest — those frames genuinely have to be
    // decoded to get there. next_frame stepping would be subtler, but it was
    // measured to be a dead letter on mkv input.
    m_correcting     = true;
    m_correctPhase   = CorrectPhase::Starting;
    m_correctSawMove = false;
    m_correctKicked  = false;
    m_correctLastMs  = libvlc_media_player_get_time(m_player);
    m_correctMuteWas = libvlc_audio_get_mute(m_player);
    m_correctRateWas = libvlc_media_player_get_rate(m_player);
    m_correctRateSet = 0.0;   // force the first set_rate in the walk
    m_correctWall.start();
    libvlc_audio_set_mute(m_player, 1);
    libvlc_media_player_play(m_player);
    m_correctTimer.start();
}

void PlaybackController::onSeekCorrectionTick()
{
    if (!m_player || !m_correcting) { m_correctTimer.stop(); return; }

    const qint64 now = libvlc_media_player_get_time(m_player);

    if (m_correctPhase == CorrectPhase::Starting) {
        if (!isPlaying()) {
            // Same wedge play() guards against: a resume after a paused seek
            // can leave the input in buffering limbo. One kick — re-issue the
            // seek and play again — revives it; a second wouldn't.
            if (!m_correctKicked && m_correctWall.elapsed() > kResumeKickMs) {
                m_correctKicked = true;
                libvlc_media_player_set_time(m_player, m_seekTargetMs);
                libvlc_media_player_play(m_player);
                return;
            }
            if (m_correctWall.elapsed() > kStartTimeoutMs) {
                finishSeekCorrection(now);   // play() never engaged
            }
            return;
        }
        m_correctPhase  = CorrectPhase::Walking;
        m_correctLastMs = now;
        return;
    }

    // Walking. If something external stopped playback out from under the walk
    // (end of file, an error), settle where we are rather than dead-waiting
    // for the wall cap.
    if (!isPlaying()) {
        finishSeekCorrection(now);
        return;
    }

    // Echo defense: right after the resume, get_time() can still
    // report the requested target while the picture is the keyframe, so
    // "now >= target" is only believed once the clock has been seen to move —
    // a moving clock is driven by decoded frames and cannot be an echo. A
    // playing pipeline always moves eventually; the wall cap covers one that
    // does not.
    if (now != m_correctLastMs) {
        m_correctLastMs  = now;
        m_correctSawMove = true;
    }

    if (m_correctSawMove && now >= m_seekTargetMs) {
        finishSeekCorrection(now);
        return;
    }
    if (m_correctWall.elapsed() > kCorrectWallCapMs) {
        finishSeekCorrection(now);
        return;
    }

    // Far from the target: hurry. Near it: crawl, so the pause command's
    // latency translates into single-digit milliseconds of media time.
    const qint64 remain = m_seekTargetMs - now;
    const double want = (remain > kApproachWindowMs) ? kCatchupRate
                                                     : kApproachRate;
    if (want != m_correctRateSet) {
        libvlc_media_player_set_rate(m_player, static_cast<float>(want));
        m_correctRateSet = want;
    }
}

void PlaybackController::finishSeekCorrection(qint64 finalMs)
{
    m_correctTimer.stop();
    libvlc_media_player_set_pause(m_player, 1);
    libvlc_media_player_set_rate(m_player, m_correctRateWas);
    if (m_correctMuteWas >= 0) {
        libvlc_audio_set_mute(m_player, m_correctMuteWas);
    }
    m_correcting   = false;
    m_correctPhase = CorrectPhase::Idle;
    emitPosition(finalMs);

    // The pause command is asynchronous, so the clock can creep another frame
    // or two before the pipeline actually stops. Read it back once at rest —
    // this is the value a mark placed here will record.
    QTimer::singleShot(kCorrectRestReadMs, this, [this]{
        if (m_player && !m_correcting) {
            emitPosition(libvlc_media_player_get_time(m_player));
        }
    });
}

void PlaybackController::cancelSeekCorrection(bool repause)
{
    m_correctTimer.stop();
    if (m_correcting) {
        // Put back everything the walk borrowed. The walk started playback;
        // unless the caller is itself switching to playing (user hit play),
        // return to the paused state the user was in.
        libvlc_media_player_set_rate(m_player, m_correctRateWas);
        if (m_correctMuteWas >= 0) {
            libvlc_audio_set_mute(m_player, m_correctMuteWas);
        }
        if (repause && isPlaying()) {
            libvlc_media_player_set_pause(m_player, 1);
        }
    }
    m_correcting   = false;
    m_correctPhase = CorrectPhase::Idle;
}

void PlaybackController::stepFrame(int direction)
{
    if (!m_player) return;
    if (direction > 0) {
        libvlc_media_player_next_frame(m_player);
    } else {
        // libVLC has no native "previous frame" — approximate via seek.
        // Assume 24 fps if we don't know better; callers can do more accurate
        // stepping by passing the actual frame duration via seekRelative().
        seekRelative(-42);
    }
}

void PlaybackController::setRate(double rate)
{
    if (m_player) libvlc_media_player_set_rate(m_player, static_cast<float>(rate));
}

qint64 PlaybackController::position() const
{
    if (!m_player) return 0;
    return libvlc_media_player_get_time(m_player);
}

qint64 PlaybackController::duration() const
{
    return m_durationCached;
}

double PlaybackController::rate() const
{
    if (!m_player) return 1.0;
    return libvlc_media_player_get_rate(m_player);
}

bool PlaybackController::isPlaying() const
{
    return m_player && libvlc_media_player_is_playing(m_player) != 0;
}

void PlaybackController::emitPosition(qint64 ms)
{
    if (ms < 0 || ms == m_lastEmittedPos) return;
    m_lastEmittedPos = ms;
    emit positionChanged(ms);
}

void PlaybackController::attachEvents()
{
    if (!m_player) return;
    libvlc_event_manager_t* em = libvlc_media_player_event_manager(m_player);
    for (auto e : kEvents) {
        libvlc_event_attach(em, e, &PlaybackController::onVlcEvent, this);
    }
}

void PlaybackController::detachEvents()
{
    if (!m_player) return;
    libvlc_event_manager_t* em = libvlc_media_player_event_manager(m_player);
    for (auto e : kEvents) {
        libvlc_event_detach(em, e, &PlaybackController::onVlcEvent, this);
    }
}

void PlaybackController::onVlcEvent(const libvlc_event_t* ev, void* opaque)
{
    auto* self = static_cast<PlaybackController*>(opaque);
    if (!self || !ev) return;

    // VLC fires events on its internal thread; marshal to the controller's thread.
    switch (ev->type) {
        case libvlc_MediaPlayerPlaying:
            QMetaObject::invokeMethod(self, [self]{
                self->m_pollTimer.start();
                emit self->playingStateChanged(true);
            }, Qt::QueuedConnection);
            break;
        case libvlc_MediaPlayerPaused:
        case libvlc_MediaPlayerStopped:
            QMetaObject::invokeMethod(self, [self]{
                self->m_pollTimer.stop();
                emit self->playingStateChanged(false);
            }, Qt::QueuedConnection);
            break;
        case libvlc_MediaPlayerEndReached:
            QMetaObject::invokeMethod(self, [self]{
                self->m_pollTimer.stop();
                emit self->endReached();
            }, Qt::QueuedConnection);
            break;
        case libvlc_MediaPlayerEncounteredError:
            QMetaObject::invokeMethod(self, [self]{
                emit self->errorOccurred(QStringLiteral("Playback error"));
            }, Qt::QueuedConnection);
            break;
        case libvlc_MediaPlayerTimeChanged: {
            const qint64 t = ev->u.media_player_time_changed.new_time;
            QMetaObject::invokeMethod(self, [self, t]{ self->emitPosition(t); },
                                      Qt::QueuedConnection);
            break;
        }
        case libvlc_MediaPlayerLengthChanged: {
            const qint64 d = ev->u.media_player_length_changed.new_length;
            QMetaObject::invokeMethod(self, [self, d]{
                self->m_durationCached = d;
                emit self->durationKnown(d);
            }, Qt::QueuedConnection);
            break;
        }
        default: break;
    }
}

} // namespace censorcut
