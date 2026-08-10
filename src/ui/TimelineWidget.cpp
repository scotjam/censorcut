#include "TimelineWidget.h"

#include "core/Marker.h"
#include "core/MarkerModel.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace censorcut {

namespace {
constexpr int kEdgeGrabPx     = 5;   // pixels around a marker edge that count as a grab
constexpr int kMiniMapTop     = 0;
constexpr int kMiniMapH       = 3;
constexpr int kMarkerBandTop  = 8;
constexpr int kMarkerBandH    = 28;
constexpr int kScrubberTop    = 42;
constexpr int kScrubberH      = 16;
constexpr int kMinHeight      = kScrubberTop + kScrubberH + 4;  // 62 px
constexpr int kHandleR        = 7;   // scrubber handle radius

// Maps a fraction to a pixel column and back. The last addressable column is
// span-1, not span — using span makes the final column unreachable when
// converting back, which shows up as the playhead never quite reaching the end.
int fracToPx(double frac, int span)
{
    return static_cast<int>(std::lround(std::clamp(frac, 0.0, 1.0) * std::max(0, span - 1)));
}

double pxToFrac(int px, int span)
{
    if (span <= 1) return 0.0;
    return std::clamp(double(px) / double(span - 1), 0.0, 1.0);
}
} // namespace

TimelineWidget::TimelineWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(kMinHeight);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setContextMenuPolicy(Qt::DefaultContextMenu);
    setFocusPolicy(Qt::StrongFocus);  // accept '0' to reset zoom when clicked on
}

void TimelineWidget::setModel(MarkerModel* model)
{
    if (m_model == model) return;
    if (m_model) m_model->disconnect(this);
    m_model = model;
    if (m_model) {
        connect(m_model, &MarkerModel::dataChanged, this, [this]{ update(); });
        connect(m_model, &MarkerModel::rowsInserted, this, [this]{ update(); });
        connect(m_model, &MarkerModel::rowsRemoved,  this, [this]{ update(); });
        connect(m_model, &MarkerModel::modelReset,   this, [this]{ update(); });
    }
    update();
}

void TimelineWidget::setDurationMs(qint64 duration)
{
    if (m_durationMs == duration) return;
    m_durationMs = duration;
    // First time we learn the duration, fit the view to the whole video.
    if (m_viewEndMs <= m_viewStartMs) {
        m_viewStartMs = 0;
        m_viewEndMs   = duration;
    } else {
        clampView();
    }
    update();
}

void TimelineWidget::resetZoom()
{
    m_viewStartMs = 0;
    m_viewEndMs   = m_durationMs;
    update();
}

void TimelineWidget::setPositionMs(qint64 position)
{
    if (m_positionMs == position) return;
    m_positionMs = position;
    update();
}

void TimelineWidget::setPendingCutStartMs(qint64 ms)
{
    if (m_pendingStartMs == ms) return;
    m_pendingStartMs = ms;
    update();
}

qint64 TimelineWidget::xToMs(int x) const
{
    if (m_viewEndMs <= m_viewStartMs || width() <= 0) return 0;
    return m_viewStartMs
           + static_cast<qint64>(pxToFrac(x, width()) * (m_viewEndMs - m_viewStartMs));
}

int TimelineWidget::msToX(qint64 ms) const
{
    if (m_viewEndMs <= m_viewStartMs) return 0;
    return fracToPx(double(ms - m_viewStartMs) / double(m_viewEndMs - m_viewStartMs), width());
}

qint64 TimelineWidget::scrubXToMs(int x) const
{
    if (m_durationMs <= 0) return 0;
    const QRect track = scrubTrackRect();
    return static_cast<qint64>(pxToFrac(x - track.left(), track.width()) * m_durationMs);
}

int TimelineWidget::msToScrubX(qint64 ms) const
{
    if (m_durationMs <= 0) return scrubTrackRect().left();
    const QRect track = scrubTrackRect();
    return track.left() + fracToPx(double(ms) / double(m_durationMs), track.width());
}

qint64 TimelineWidget::miniMapXToMs(int x) const
{
    if (m_durationMs <= 0) return 0;
    return static_cast<qint64>(pxToFrac(x, width()) * m_durationMs);
}

void TimelineWidget::clampView()
{
    if (m_durationMs <= 0) return;
    const qint64 minSpan = 100;  // never let the view collapse below 0.1s
    qint64 span = std::max<qint64>(minSpan, m_viewEndMs - m_viewStartMs);
    span = std::min<qint64>(span, m_durationMs);
    if (m_viewStartMs < 0) m_viewStartMs = 0;
    if (m_viewStartMs + span > m_durationMs) m_viewStartMs = m_durationMs - span;
    if (m_viewStartMs < 0) m_viewStartMs = 0;
    m_viewEndMs = m_viewStartMs + span;
}

void TimelineWidget::zoomBy(double factor, int anchorX)
{
    if (m_durationMs <= 0) return;
    const qint64 anchorMs = xToMs(anchorX);
    const qint64 oldSpan  = std::max<qint64>(1, m_viewEndMs - m_viewStartMs);
    qint64 newSpan = static_cast<qint64>(double(oldSpan) / factor);
    newSpan = std::clamp<qint64>(newSpan, 100, m_durationMs);

    // Keep the timestamp under the cursor stationary on screen.
    const double anchorFrac = double(anchorX) / std::max(1, width());
    m_viewStartMs = anchorMs - static_cast<qint64>(anchorFrac * newSpan);
    m_viewEndMs   = m_viewStartMs + newSpan;
    clampView();
    update();
}

void TimelineWidget::panBy(qint64 deltaMs)
{
    m_viewStartMs += deltaMs;
    m_viewEndMs   += deltaMs;
    clampView();
    update();
}

QRect TimelineWidget::miniMapRect() const
{
    // The drawn strip is only kMiniMapH tall, which is too thin to hit
    // reliably; the clickable band extends down to the marker band.
    return QRect(0, kMiniMapTop, width(), kMarkerBandTop - kMiniMapTop);
}

QRect TimelineWidget::markerBandRect() const
{
    return QRect(0, kMarkerBandTop, width(), kMarkerBandH);
}

QRect TimelineWidget::scrubberRect() const
{
    return QRect(0, kScrubberTop, width(), kScrubberH);
}

QRect TimelineWidget::scrubTrackRect() const
{
    // Inset by the handle radius so the handle's centre can travel the full
    // track without the ellipse clipping — no clamping, so the handle always
    // sits exactly on the position the fill indicates.
    return scrubberRect().adjusted(kHandleR, 0, -kHandleR, 0);
}

TimelineWidget::Hit TimelineWidget::hitTest(const QPoint& pos) const
{
    Hit hit;
    if (!m_model || m_durationMs <= 0) return hit;
    if (!markerBandRect().contains(pos)) return hit;

    // Iterate in reverse so a marker drawn last (on top) wins ties.
    const auto& markers = m_model->markers();
    for (auto it = markers.crbegin(); it != markers.crend(); ++it) {
        const int x1 = msToX(it->startMs);
        const int x2 = msToX(it->endMs);
        if (std::abs(pos.x() - x1) <= kEdgeGrabPx) { hit.id = it->id; hit.edge = Edge::Start; return hit; }
        if (std::abs(pos.x() - x2) <= kEdgeGrabPx) { hit.id = it->id; hit.edge = Edge::End;   return hit; }
        if (pos.x() > x1 && pos.x() < x2)          { hit.id = it->id; hit.body = true;        return hit; }
    }
    return hit;
}

void TimelineWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(0x22, 0x24, 0x2A));

    const QRect band = markerBandRect();
    const QRect scrub = scrubberRect();

    // Marker band background
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x2D, 0x30, 0x37));
    p.drawRect(band);

    // Markers — skip ones that are entirely outside the visible view.
    if (m_model && m_durationMs > 0 && m_viewEndMs > m_viewStartMs) {
        for (const auto& m : m_model->markers()) {
            if (m.endMs <= m_viewStartMs || m.startMs >= m_viewEndMs) continue;
            const int x1 = msToX(m.startMs);
            const int x2 = msToX(m.endMs);
            const int w  = std::max(2, x2 - x1);
            QColor c;
            switch (m.status) {
                case Status::Confirmed: c = QColor(0xE3, 0x4A, 0x4A, 220); break;  // red
                case Status::Pending:   c = QColor(0xE0, 0xA8, 0x33, 200); break;  // amber
                case Status::Rejected:  c = QColor(0x66, 0x66, 0x66, 120); break;  // grey
            }
            p.setBrush(c);
            p.drawRect(x1, band.top(), w, band.height());
        }
    }

    // Pending start handle (after `[` was pressed)
    if (m_pendingStartMs >= 0 && m_durationMs > 0) {
        const int x = msToX(m_pendingStartMs);
        p.setPen(QPen(QColor(0xFF, 0xC8, 0x4D), 2));
        p.drawLine(x, band.top() - 2, x, scrub.bottom() + 2);
        p.setBrush(QColor(0xFF, 0xC8, 0x4D, 60));
        p.setPen(Qt::NoPen);
        p.drawRect(x, band.top(), msToX(m_positionMs) - x, band.height());
    }

    // Scrubber strip — the fat horizontal bar at the bottom dedicated to
    // seeking. Distinct from the marker band so dragging here can never
    // accidentally edit a marker's edge.
    //
    // This strip is ALWAYS absolute over the whole film: it reads as a
    // progress bar, so mapping it through the zoom view would leave the handle
    // showing position-within-window while looking like position-within-film.
    // Zoom belongs to the marker band above.
    const QRect track = scrubTrackRect();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x18, 0x1A, 0x20));
    p.drawRoundedRect(scrub.adjusted(2, 0, -2, 0), 4, 4);

    if (m_durationMs > 0) {
        const int playX = msToScrubX(m_positionMs);
        // Filled portion left of the playhead.
        const QRect fill(track.left(), scrub.top() + 2,
                         std::max(0, playX - track.left()), scrub.height() - 4);
        p.setBrush(QColor(0x4A, 0x90, 0xE2));
        p.drawRoundedRect(fill, 3, 3);

        // White circular handle so the user can see the grab point. The track
        // is inset by kHandleR, so this never needs clamping.
        p.setPen(QPen(QColor(0x2A, 0x60, 0xA0), 1));
        p.setBrush(Qt::white);
        p.drawEllipse(QPoint(playX, scrub.center().y() + 1), kHandleR, kHandleR);
    }

    // Playhead vertical line through the marker band (helps line up cuts
    // visually). Skip if outside the current view.
    if (m_durationMs > 0 && m_positionMs >= m_viewStartMs && m_positionMs <= m_viewEndMs) {
        const int x = msToX(m_positionMs);
        p.setPen(QPen(QColor(0xF0, 0xF0, 0xF0), 2));
        p.drawLine(x, band.top() - 2, x, band.bottom() + 2);
    }

    // Mini-map / zoom indicator across the very top (3 px high). Drawn
    // whenever we know the duration, because it doubles as a click target for
    // jumping anywhere in the film while zoomed in.
    if (m_durationMs > 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x40, 0x44, 0x4C));
        p.drawRect(0, kMiniMapTop, width(), kMiniMapH);

        const int xs = fracToPx(double(m_viewStartMs) / double(m_durationMs), width());
        const int xe = fracToPx(double(m_viewEndMs)   / double(m_durationMs), width());
        p.setBrush(QColor(0xFF, 0xC8, 0x4D));
        p.drawRect(xs, kMiniMapTop, std::max(2, xe - xs), kMiniMapH);

        if (m_viewStartMs > 0 || m_viewEndMs < m_durationMs) {
            const double zoomLevel = double(m_durationMs)
                                     / std::max<qint64>(1, m_viewEndMs - m_viewStartMs);
            p.setPen(QColor(0xFF, 0xC8, 0x4D));
            p.drawText(width() - 64, kMarkerBandTop + 2,
                       QStringLiteral("%1× zoom").arg(zoomLevel, 0, 'f', 1));
        }
    }
}

void TimelineWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || m_durationMs <= 0) return;

    const QPoint pos = event->pos();

    // Route by band. Each band has its own coordinate space, so falling
    // through to a single "scrub anything that isn't a marker edge" branch
    // seeks to the wrong time — most visibly on the mini-map, which is drawn
    // in absolute time but was being converted view-relative.
    if (scrubberRect().contains(pos)) {
        m_scrubSpace = ScrubSpace::Scrubber;
        emit scrubBegan();
        emit scrubbed(scrubXToMs(pos.x()));
        return;
    }

    if (miniMapRect().contains(pos)) {
        m_scrubSpace = ScrubSpace::MiniMap;
        emit scrubBegan();
        emit scrubbed(miniMapXToMs(pos.x()));
        return;
    }

    if (markerBandRect().contains(pos)) {
        const Hit hit = hitTest(pos);
        if (hit.edge != Edge::None) {
            m_dragId   = hit.id;
            m_dragEdge = hit.edge;
            return;  // start an edge-drag; don't scrub
        }
        m_scrubSpace = ScrubSpace::View;
        emit scrubBegan();
        emit scrubbed(xToMs(pos.x()));
    }
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_durationMs <= 0) return;

    if (m_dragEdge != Edge::None && (event->buttons() & Qt::LeftButton) && m_model) {
        auto m = m_model->findById(m_dragId);
        if (!m) { m_dragEdge = Edge::None; return; }

        const qint64 newMs = xToMs(event->pos().x());
        Marker updated = *m;
        if (m_dragEdge == Edge::Start) {
            updated.startMs = std::clamp<qint64>(newMs, 0, updated.endMs - 1);
        } else {
            updated.endMs = std::clamp<qint64>(newMs, updated.startMs + 1,
                                               m_durationMs > 0 ? m_durationMs : newMs);
        }
        m_model->updateMarkerById(m_dragId, updated);
        return;
    }

    // A scrub drag stays in whichever space it started in, even once the
    // cursor wanders out of that band.
    if (m_scrubSpace != ScrubSpace::None && (event->buttons() & Qt::LeftButton)) {
        const int x = event->pos().x();
        switch (m_scrubSpace) {
            case ScrubSpace::Scrubber: emit scrubbed(scrubXToMs(x));   break;
            case ScrubSpace::MiniMap:  emit scrubbed(miniMapXToMs(x)); break;
            case ScrubSpace::View:     emit scrubbed(xToMs(x));        break;
            case ScrubSpace::None:     break;
        }
        return;
    }

    // No buttons held — show a horizontal-resize cursor when hovering near an edge.
    const Hit hover = hitTest(event->pos());
    setCursor(hover.edge != Edge::None ? Qt::SizeHorCursor : Qt::ArrowCursor);
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragEdge = Edge::None;
        m_dragId = QUuid();
        if (m_scrubSpace != ScrubSpace::None) {
            m_scrubSpace = ScrubSpace::None;
            emit scrubEnded();
        }
    }
}

void TimelineWidget::wheelEvent(QWheelEvent* event)
{
    if (m_durationMs <= 0) { event->ignore(); return; }
    const int delta = event->angleDelta().y();
    if (delta == 0) { event->ignore(); return; }

    const auto mods = event->modifiers();
    const QPoint pos = event->position().toPoint();

    if (mods.testFlag(Qt::ControlModifier)) {
        // Ctrl+wheel: zoom centred on cursor. 1.2x per notch (120 units).
        const double factor = std::pow(1.2, double(delta) / 120.0);
        zoomBy(factor, pos.x());
        event->accept();
    } else if (mods.testFlag(Qt::ShiftModifier)) {
        // Shift+wheel: pan by ~10% of the visible span per notch.
        const qint64 span = std::max<qint64>(1, m_viewEndMs - m_viewStartMs);
        const qint64 step = static_cast<qint64>(span * 0.1);
        panBy(delta > 0 ? -step : +step);
        event->accept();
    } else {
        event->ignore();
    }
}

void TimelineWidget::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_0 && !event->modifiers().testFlag(Qt::ControlModifier)) {
        resetZoom();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent* event)
{
    if (!m_model || m_durationMs <= 0) return;
    const Hit hit = hitTest(event->pos());
    if (hit.id.isNull()) return;
    showMarkerMenu(hit.id, event->globalPos());
    event->accept();
}

void TimelineWidget::showMarkerMenu(const QUuid& id, const QPoint& globalPos)
{
    auto opt = m_model->findById(id);
    if (!opt) return;
    const Marker m = *opt;

    QMenu menu(this);
    QAction* confirm = menu.addAction(tr("Confirm"));
    QAction* reject  = menu.addAction(tr("Reject"));
    menu.addSeparator();
    QAction* del     = menu.addAction(tr("Delete cut"));

    confirm->setEnabled(m.status != Status::Confirmed);
    reject->setEnabled(m.status != Status::Rejected);

    QAction* chosen = menu.exec(globalPos);
    if (!chosen) return;

    if (chosen == del) {
        m_model->removeMarkerById(id);
    } else if (chosen == confirm || chosen == reject) {
        Marker updated = m;
        updated.status = (chosen == confirm) ? Status::Confirmed : Status::Rejected;
        m_model->updateMarkerById(id, updated);
    }
}

} // namespace censorcut
