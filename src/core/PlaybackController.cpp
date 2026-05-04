#include "PlaybackController.h"

#include <QDebug>
#include <QFileInfo>
#include <QMetaObject>
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

} // namespace

PlaybackController::PlaybackController(QObject* parent)
    : QObject(parent)
{
    // No options passed — VLC will use defaults. Add `--no-video-title-show`
    // here if the title overlay is annoying during playback.
    static const char* args[] = { "--no-video-title-show", "--quiet" };
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

    if (m_media) {
        libvlc_media_release(m_media);
        m_media = nullptr;
    }

    const QByteArray utf8 = QFileInfo(path).absoluteFilePath().toUtf8();
    m_media = libvlc_media_new_path(m_vlc, utf8.constData());
    if (!m_media) {
        emit errorOccurred(QStringLiteral("libvlc_media_new_path failed for %1").arg(path));
        return false;
    }
    libvlc_media_player_set_media(m_player, m_media);
    m_durationCached = 0;
    emit mediaOpened(path);
    return true;
}

void PlaybackController::play()
{
    if (m_player) libvlc_media_player_play(m_player);
}

void PlaybackController::pause()
{
    if (m_player) libvlc_media_player_set_pause(m_player, 1);
}

void PlaybackController::togglePlayPause()
{
    if (!m_player) return;
    if (libvlc_media_player_is_playing(m_player))
        libvlc_media_player_set_pause(m_player, 1);
    else
        libvlc_media_player_play(m_player);
}

void PlaybackController::stop()
{
    if (m_player) libvlc_media_player_stop(m_player);
}

void PlaybackController::seek(qint64 ms)
{
    if (!m_player) return;
    libvlc_media_player_set_time(m_player, ms);
}

void PlaybackController::seekRelative(qint64 deltaMs)
{
    seek(position() + deltaMs);
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
            QMetaObject::invokeMethod(self, [self]{ emit self->playingStateChanged(true); },
                                      Qt::QueuedConnection);
            break;
        case libvlc_MediaPlayerPaused:
        case libvlc_MediaPlayerStopped:
            QMetaObject::invokeMethod(self, [self]{ emit self->playingStateChanged(false); },
                                      Qt::QueuedConnection);
            break;
        case libvlc_MediaPlayerEndReached:
            QMetaObject::invokeMethod(self, [self]{ emit self->endReached(); },
                                      Qt::QueuedConnection);
            break;
        case libvlc_MediaPlayerEncounteredError:
            QMetaObject::invokeMethod(self, [self]{
                emit self->errorOccurred(QStringLiteral("Playback error"));
            }, Qt::QueuedConnection);
            break;
        case libvlc_MediaPlayerTimeChanged: {
            const qint64 t = ev->u.media_player_time_changed.new_time;
            QMetaObject::invokeMethod(self, [self, t]{ emit self->positionChanged(t); },
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
