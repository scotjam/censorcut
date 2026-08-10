// Coordinate-mapping tests for TimelineWidget.
//
// The widget has three horizontal bands with two different coordinate spaces
// (see TimelineWidget.h). The bug these cover: the scrubber strip and the
// mini-map were both being hit-tested through the zoom view, so once the user
// zoomed in, clicking the "progress bar" seeked to a position within the
// visible window rather than within the film.
//
// Everything is driven through synthesized mouse events and the scrubbed()
// signal, so the mappings stay private.

#include "ui/TimelineWidget.h"

#include <QApplication>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QWheelEvent>
#include <QtTest>

using namespace censorcut;

namespace {

// Must match the band layout constants in TimelineWidget.cpp.
constexpr int kWidgetW    = 400;
constexpr int kWidgetH    = 62;
constexpr int kHandleR    = 7;
constexpr int kMiniMapY   = 4;    // inside the mini-map band (0..7)
constexpr int kBandY      = 20;   // inside the marker band (8..35)
constexpr int kGapY       = 38;   // dead space between the bands
constexpr int kScrubberY  = 50;   // inside the scrubber strip (42..57)

constexpr qint64 kDurationMs = 1200000;  // 20 minutes

void click(QWidget* w, int x, int y)
{
    const QPointF p(x, y);
    QMouseEvent press(QEvent::MouseButtonPress, p, w->mapToGlobal(p),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, p, w->mapToGlobal(p),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(w, &release);
}

void drag(QWidget* w, int fromX, int toX, int y)
{
    const QPointF a(fromX, y), b(toX, y);
    QMouseEvent press(QEvent::MouseButtonPress, a, w->mapToGlobal(a),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
    QMouseEvent move(QEvent::MouseMove, b, w->mapToGlobal(b),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, b, w->mapToGlobal(b),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(w, &release);
}

/// Ctrl+wheel one notch up, anchored at x. Anchoring at 0 keeps the view start
/// at 0 and shrinks the span, which makes the view/absolute divergence easy to
/// assert on.
void zoomIn(QWidget* w, int anchorX)
{
    const QPointF p(anchorX, kBandY);
    QWheelEvent ev(p, w->mapToGlobal(p), QPoint(0, 0), QPoint(0, 120),
                   Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(w, &ev);
}

qint64 lastScrub(const QSignalSpy& spy)
{
    return spy.isEmpty() ? -1 : spy.last().at(0).toLongLong();
}

} // namespace

class TestTimelineWidget : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        m_w = new TimelineWidget;
        m_w->resize(kWidgetW, kWidgetH);
        m_w->setDurationMs(kDurationMs);
    }

    void cleanup() { delete m_w; m_w = nullptr; }

    void scrubberEndpointsAreReachable()
    {
        QSignalSpy spy(m_w, &TimelineWidget::scrubbed);

        click(m_w, kHandleR, kScrubberY);
        QCOMPARE(lastScrub(spy), 0);

        // The track is inset by the handle radius at both ends so the handle
        // never needs clamping; its right edge must map to the full duration.
        click(m_w, kWidgetW - 1 - kHandleR, kScrubberY);
        QCOMPARE(lastScrub(spy), kDurationMs);
    }

    void scrubberIsAbsoluteWhileZoomed()
    {
        QSignalSpy spy(m_w, &TimelineWidget::scrubbed);
        const int midX = kWidgetW / 2;

        click(m_w, midX, kScrubberY);
        const qint64 unzoomed = lastScrub(spy);
        QVERIFY(qAbs(unzoomed - kDurationMs / 2) < kDurationMs / 100);

        for (int i = 0; i < 5; ++i) zoomIn(m_w, 0);  // ~2.5x, view start stays 0

        click(m_w, midX, kScrubberY);
        QCOMPARE(lastScrub(spy), unzoomed);
    }

    void miniMapIsAbsoluteWhileZoomed()
    {
        QSignalSpy spy(m_w, &TimelineWidget::scrubbed);

        for (int i = 0; i < 5; ++i) zoomIn(m_w, 0);

        click(m_w, 0, kMiniMapY);
        QCOMPARE(lastScrub(spy), 0);

        click(m_w, kWidgetW - 1, kMiniMapY);
        QCOMPARE(lastScrub(spy), kDurationMs);

        // The mini-map is painted in absolute time, so a click three quarters
        // along it must land three quarters through the film — not three
        // quarters through the zoomed window.
        click(m_w, (kWidgetW - 1) * 3 / 4, kMiniMapY);
        QVERIFY(qAbs(lastScrub(spy) - kDurationMs * 3 / 4) < kDurationMs / 100);
    }

    void markerBandStaysViewRelative()
    {
        QSignalSpy spy(m_w, &TimelineWidget::scrubbed);
        const int midX = kWidgetW / 2;

        click(m_w, midX, kBandY);
        QVERIFY(qAbs(lastScrub(spy) - kDurationMs / 2) < kDurationMs / 100);

        // One notch anchored at 0: span becomes duration/1.2, start stays 0.
        zoomIn(m_w, 0);
        click(m_w, midX, kBandY);
        const qint64 expected = qint64(kDurationMs / 1.2 / 2);
        QVERIFY(qAbs(lastScrub(spy) - expected) < kDurationMs / 100);
    }

    void dragKeepsTheSpaceItStartedIn()
    {
        QSignalSpy spy(m_w, &TimelineWidget::scrubbed);
        for (int i = 0; i < 5; ++i) zoomIn(m_w, 0);

        // Press on the scrubber, then drag up into the marker band. The drag
        // must keep using absolute coordinates rather than switching spaces
        // under the cursor.
        const QPointF a(kHandleR, kScrubberY);
        QMouseEvent press(QEvent::MouseButtonPress, a, m_w->mapToGlobal(a),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(m_w, &press);

        const QPointF b(kWidgetW - 1 - kHandleR, kBandY);
        QMouseEvent move(QEvent::MouseMove, b, m_w->mapToGlobal(b),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(m_w, &move);

        QCOMPARE(lastScrub(spy), kDurationMs);
    }

    void deadSpaceDoesNotSeek()
    {
        QSignalSpy scrubbed(m_w, &TimelineWidget::scrubbed);
        QSignalSpy began(m_w, &TimelineWidget::scrubBegan);
        QSignalSpy ended(m_w, &TimelineWidget::scrubEnded);

        click(m_w, kWidgetW / 2, kGapY);
        QCOMPARE(scrubbed.count(), 0);
        QCOMPARE(began.count(), 0);
        // scrubEnded must be paired with scrubBegan — MainWindow uses the pair
        // to bracket m_userScrubbing.
        QCOMPARE(ended.count(), 0);
    }

    void scrubSignalsArePaired()
    {
        QSignalSpy began(m_w, &TimelineWidget::scrubBegan);
        QSignalSpy ended(m_w, &TimelineWidget::scrubEnded);

        drag(m_w, kHandleR, kWidgetW / 2, kScrubberY);
        QCOMPARE(began.count(), 1);
        QCOMPARE(ended.count(), 1);
    }

    void noDurationMeansNoSeek()
    {
        TimelineWidget w;
        w.resize(kWidgetW, kWidgetH);
        QSignalSpy spy(&w, &TimelineWidget::scrubbed);
        click(&w, kWidgetW / 2, kScrubberY);
        QCOMPARE(spy.count(), 0);
    }

private:
    TimelineWidget* m_w = nullptr;
};

QTEST_MAIN(TestTimelineWidget)
#include "test_timeline_widget.moc"
