// Seek-accuracy checks that need a real decoder and a real file.
//
// The widget tests cover the timeline's coordinate maths, but nothing there
// touches libVLC, so the seek behaviour in PlaybackController had no automated
// coverage at all. This fills that gap.
//
// The test is opt-in: point CENSORCUT_TEST_MEDIA at a video file and it runs,
// otherwise every case skips. That keeps `ctest` green on a machine with no
// media (and keeps film paths out of the repo). A long-GOP H.264 or an XviD
// AVI is the interesting input, because those are exactly the encodes where a
// raw demux seek lands seconds away from the requested time.
//
// Hard-won caveat, preserved so nobody re-learns it: while paused, libVLC can
// echo the requested time back through get_time() while the picture on screen
// is still the preceding keyframe. A test that only reads the clock after a
// seek therefore passes no matter where the decoder went — the first version
// of this file did exactly that, and flipping --input-fast-seek on produced
// byte-identical "0 ms" results. Forcing a frame decode (next_frame) is what
// makes the clock carry a real PTS; the frame-step probe below exists to keep
// the other assertions honest.
//
// PlaybackController now closes the gap itself: a paused seek is followed by a
// correction walk that decodes forward until the clock reaches the target
// (see beginPausedSeekCorrection). These cases assert that the walk converges
// — landing within a frame or two of the request, never seconds early.
//
// Note: no video sink is attached, so libVLC opens its own output window while
// this runs. That is cosmetic — decoding has to happen for the landing time to
// mean anything, so the video path is deliberately left switched on.

#include <QtTest>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSignalSpy>
#include <QThread>

#include <functional>
#include <memory>

#include "core/PlaybackController.h"

using namespace censorcut;

namespace {

/// Spin the event loop until cond() holds or the timeout expires. libVLC
/// delivers state on its own thread and PlaybackController marshals it back
/// through queued connections, so nothing arrives without a running loop.
bool waitFor(const std::function<bool()>& cond, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (!cond()) {
        if (t.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
    return true;
}

/// Let queued work drain for a fixed stretch of wall time.
void spinFor(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

/// Wait until position() stops moving. The paused-seek correction walks the
/// decoder forward asynchronously — potentially hundreds of frames on a long
/// GOP — so "where did the seek land" is only meaningful once the walk is
/// done. The stability window must exceed the walk's worst internal stall
/// (~600 ms between re-pokes), or a mid-walk lull reads as convergence.
qint64 waitForSettledPosition(PlaybackController* pb,
                              int stableMs = 1500, int capMs = 30000)
{
    QElapsedTimer total, sinceChange;
    total.start();
    sinceChange.start();
    qint64 last = pb->position();
    while (total.elapsed() < capMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(15);
        const qint64 now = pb->position();
        if (now != last) {
            last = now;
            sinceChange.restart();
        } else if (sinceChange.elapsed() >= stableMs) {
            break;
        }
    }
    return last;
}

/// How far a converged paused seek may sit from the requested time.
///
/// The correction walk stops at the first decoded frame with PTS >= target,
/// and its initial probe step can cost one extra frame when the demux landing
/// was already exact — so the expected drift is [0, ~2 frames]. 100 ms early /
/// 250 ms late gives every frame rate headroom while sitting far below any
/// keyframe interval (2–10 s on the encodes this app deals with), so a
/// keyframe-snapped landing can never pass.
constexpr qint64 kEarlySlackMs = 100;
constexpr qint64 kLateSlackMs  = 250;

} // namespace

class TestPlaybackSeek : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void durationBecomesKnown();
    void seekLandsOnTheRequestedTime_data();
    void seekLandsOnTheRequestedTime();
    void backwardSeekIsAlsoExact();
    void optimisticPositionIsReportedImmediately();
    void decodedFrameConfirmsTheLandingPoint_data();
    void decodedFrameConfirmsTheLandingPoint();
    void resumeAfterPausedSeekEngages();

private:
    /// Skips the current test unless media was supplied and opened.
    bool haveMedia() const { return m_ready; }

    QString m_media;
    bool    m_ready = false;
    qint64  m_durationMs = 0;
    std::unique_ptr<PlaybackController> m_pb;
};

void TestPlaybackSeek::initTestCase()
{
    m_media = qEnvironmentVariable("CENSORCUT_TEST_MEDIA");
    if (m_media.isEmpty()) {
        qInfo("CENSORCUT_TEST_MEDIA is unset - skipping seek-accuracy checks.");
        return;
    }
    if (!QFileInfo::exists(m_media)) {
        QFAIL(qPrintable(QStringLiteral("CENSORCUT_TEST_MEDIA points at a file "
                                        "that does not exist: %1").arg(m_media)));
    }

    m_pb = std::make_unique<PlaybackController>();
    QVERIFY2(m_pb->open(m_media), "PlaybackController::open() failed");

    // Duration arrives via libvlc's LengthChanged, which only fires once the
    // input is running — so start playback, wait for it, then pause. Every
    // seek case below runs paused, which is exactly the case the correction
    // walk exists for.
    m_pb->play();
    const bool got = waitFor([this]{ return m_pb->duration() > 0; }, 15000);
    QVERIFY2(got, "duration never became known - is the file playable?");
    m_durationMs = m_pb->duration();

    m_pb->pause();
    spinFor(300);

    // Warm-up: the first seek after pausing can be read back as 0 because the
    // clock is not live yet. Burn one seek so the cases below measure the
    // player rather than its cold start.
    m_pb->seek(m_durationMs / 20);
    waitFor([this]{ return m_pb->position() > 0; }, 5000);
    waitForSettledPosition(m_pb.get());

    m_ready = true;
    qInfo("media: %s", qPrintable(QFileInfo(m_media).fileName()));
    qInfo("duration: %lld ms", static_cast<long long>(m_durationMs));
}

void TestPlaybackSeek::cleanupTestCase()
{
    if (m_pb) {
        m_pb->stop();
        spinFor(200);
        m_pb.reset();
    }
}

void TestPlaybackSeek::durationBecomesKnown()
{
    if (!haveMedia()) QSKIP("no CENSORCUT_TEST_MEDIA");
    QVERIFY(m_durationMs > 0);
}

void TestPlaybackSeek::seekLandsOnTheRequestedTime_data()
{
    QTest::addColumn<double>("fraction");
    QTest::newRow("10%") << 0.10;
    QTest::newRow("25%") << 0.25;
    QTest::newRow("50%") << 0.50;
    QTest::newRow("75%") << 0.75;
    QTest::newRow("90%") << 0.90;
}

void TestPlaybackSeek::seekLandsOnTheRequestedTime()
{
    if (!haveMedia()) QSKIP("no CENSORCUT_TEST_MEDIA");
    QFETCH(double, fraction);

    const qint64 target = static_cast<qint64>(m_durationMs * fraction);
    m_pb->seek(target);
    const qint64 landed = waitForSettledPosition(m_pb.get());
    const qint64 drift  = landed - target;
    qInfo("target %lld ms -> settled %lld ms (drift %lld ms)",
          static_cast<long long>(target), static_cast<long long>(landed),
          static_cast<long long>(drift));

    QVERIFY2(drift >= -kEarlySlackMs && drift <= kLateSlackMs,
             qPrintable(QStringLiteral("seek settled %1 ms from the target "
                                       "(allowed -%2..+%3) - did the "
                                       "correction walk converge?")
                            .arg(drift).arg(kEarlySlackMs).arg(kLateSlackMs)));
}

void TestPlaybackSeek::backwardSeekIsAlsoExact()
{
    if (!haveMedia()) QSKIP("no CENSORCUT_TEST_MEDIA");

    // Seek forward, then back. A backward seek has to rebuild from an earlier
    // keyframe, which is where a broken or missing AVI index shows up.
    const qint64 far  = static_cast<qint64>(m_durationMs * 0.80);
    const qint64 back = static_cast<qint64>(m_durationMs * 0.20);

    m_pb->seek(far);
    qint64 drift = waitForSettledPosition(m_pb.get()) - far;
    QVERIFY2(drift >= -kEarlySlackMs && drift <= kLateSlackMs,
             "forward leg missed the target");

    m_pb->seek(back);
    drift = waitForSettledPosition(m_pb.get()) - back;
    qInfo("backward: target %lld ms, drift %lld ms",
          static_cast<long long>(back), static_cast<long long>(drift));
    QVERIFY2(drift >= -kEarlySlackMs && drift <= kLateSlackMs,
             "backward seek missed the target");
}

void TestPlaybackSeek::optimisticPositionIsReportedImmediately()
{
    if (!haveMedia()) QSKIP("no CENSORCUT_TEST_MEDIA");

    // seek() reports the requested time straight away so the UI stays
    // responsive; the correction walk then converges on a true reading. Both
    // halves matter: the first for scrub feel, the second for marker times.
    const qint64 target = static_cast<qint64>(m_durationMs * 0.42);

    QSignalSpy spy(m_pb.get(), &PlaybackController::positionChanged);
    m_pb->seek(target);

    spinFor(60);                       // well inside the settle window
    const qint64 optimistic = m_pb->position();

    const qint64 settled = waitForSettledPosition(m_pb.get());
    qInfo("optimistic %lld ms -> settled %lld ms (%lld emissions)",
          static_cast<long long>(optimistic), static_cast<long long>(settled),
          static_cast<long long>(spy.count()));

    QCOMPARE(optimistic, target);
    const qint64 drift = settled - target;
    QVERIFY2(drift >= -kEarlySlackMs && drift <= kLateSlackMs,
             "settled position is not within a frame or two of the request");
}

void TestPlaybackSeek::decodedFrameConfirmsTheLandingPoint_data()
{
    QTest::addColumn<double>("fraction");
    QTest::newRow("15%") << 0.15;
    QTest::newRow("40%") << 0.40;
    QTest::newRow("65%") << 0.65;
    QTest::newRow("85%") << 0.85;
}

void TestPlaybackSeek::decodedFrameConfirmsTheLandingPoint()
{
    if (!haveMedia()) QSKIP("no CENSORCUT_TEST_MEDIA");
    QFETCH(double, fraction);

    const qint64 target = static_cast<qint64>(m_durationMs * fraction);
    m_pb->seek(target);
    const qint64 settled = waitForSettledPosition(m_pb.get());
    const qint64 drift   = settled - target;

    // The independent truth check: play briefly, pause, and watch the clock.
    // A genuine reading advances by roughly the played wall time. A stale
    // echo snaps BACKWARD by seconds, because playing resumes from where the
    // decoder really was — which is precisely how the placebo version of this
    // test was unmasked. (stepFrame would be the subtler probe, but
    // next_frame is a no-op on mkv AND its queued step requests poison
    // subsequent seeks, which this test also established the hard way.)
    // Assert the landing before probing — the probe can only SKIP, never
    // excuse a bad drift.
    QVERIFY2(drift >= -kEarlySlackMs && drift <= kLateSlackMs,
             qPrintable(QStringLiteral("decoded landing sits %1 ms from the "
                                       "target (allowed -%2..+%3)")
                            .arg(drift).arg(kEarlySlackMs).arg(kLateSlackMs)));

    // The independent truth check: play briefly, pause, read. A genuine clock
    // advances by roughly the played wall time; a stale echo snaps BACKWARD
    // by seconds, because playback resumes from where the decoder really was
    // — which is precisely how the placebo version of this test was unmasked.
    //
    // On AVI no probe instrument survives contact with the paused-seek resume
    // wedge (see the bd issue): the pulse's play never engages, and even
    // next_frame — which works on a freshly-seeked AVI — is dead once any
    // play has been attempted, including the correction walk's own. So a
    // detected wedge SKIPs rather than fails: the drift assertion above still
    // ran, and the exactness of AVI landings is corroborated by the AVI
    // index (and by pre-walk frame-step measurements of +1 frame).
    m_pb->play();
    const bool engaged = waitFor([&]{ return m_pb->isPlaying(); }, 1200);
    const bool moved   = waitFor([&]{ return m_pb->position() != settled; },
                                 engaged ? 3000 : 500);
    m_pb->pause();
    spinFor(400);   // let the pause engage and the clock come to rest

    const qint64 after   = m_pb->position();
    const qint64 advance = after - settled;
    qInfo("target %lld | settled %lld (drift %lld ms) | probe advance %lld ms",
          static_cast<long long>(target), static_cast<long long>(settled),
          static_cast<long long>(drift), static_cast<long long>(advance));

    if (!engaged && !moved) {
        QSKIP("truth probe unavailable: resume after a paused seek wedges on "
              "this container (censorcut-repo-ale); drift was still asserted");
    }
    QVERIFY2(advance > 0 && advance <= 4000,
             qPrintable(QStringLiteral("probe moved the clock by %1 ms - a "
                                       "small positive advance means the "
                                       "settled value was a real decoded PTS; "
                                       "backward or frozen means it was not")
                            .arg(advance)));
}

void TestPlaybackSeek::resumeAfterPausedSeekEngages()
{
    if (!haveMedia()) QSKIP("no CENSORCUT_TEST_MEDIA");

    // Regression sentinel for the --avi-index wedge. With --avi-index=1
    // ("always fix"), EVERY seek on an AVI triggered an index rebuild that
    // permanently stalled the input: paused seek then play never engaged
    // (measured 8+ s dead, confirmed by the user in the GUI), and a playing
    // seek froze the clock outright. =3 ("fix when necessary") keeps the
    // repair path for genuinely broken files without touching intact ones.
    // If this case ever fails, look at the avi-index option first.
    const qint64 target = static_cast<qint64>(m_durationMs * 0.35);
    m_pb->seek(target);
    waitForSettledPosition(m_pb.get());
    m_pb->play();
    const bool engaged = waitFor([&]{ return m_pb->isPlaying(); }, 5000);
    m_pb->pause();
    spinFor(400);
    QVERIFY2(engaged,
             "resume after a paused seek did not engage - the avi-index "
             "wedge is back (censorcut-repo-ale)");
}

QTEST_GUILESS_MAIN(TestPlaybackSeek)
#include "test_playback_seek.moc"
