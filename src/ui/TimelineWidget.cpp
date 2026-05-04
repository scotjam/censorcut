#include "TimelineWidget.h"

#include "core/Marker.h"
#include "core/MarkerModel.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QtMath>

namespace censorcut {

namespace {
constexpr int kEdgeGrabPx = 5;        // pixels around a marker edge that count as a grab
constexpr int kBandHalfHeight = 12;   // marker band extends ±this from the vertical centre
} // namespace

TimelineWidget::TimelineWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(48);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setContextMenuPolicy(Qt::DefaultContextMenu);
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
    if (m_durationMs <= 0 || width() <= 0) return 0;
    const double frac = std::clamp(double(x) / width(), 0.0, 1.0);
    return static_cast<qint64>(frac * m_durationMs);
}

int TimelineWidget::msToX(qint64 ms) const
{
    if (m_durationMs <= 0) return 0;
    const double frac = std::clamp(double(ms) / m_durationMs, 0.0, 1.0);
    return static_cast<int>(frac * width());
}

TimelineWidget::Hit TimelineWidget::hitTest(const QPoint& pos) const
{
    Hit hit;
    if (!m_model || m_durationMs <= 0) return hit;

    const int yMid = height() / 2;
    if (pos.y() < yMid - kBandHalfHeight || pos.y() > yMid + kBandHalfHeight) return hit;

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

    // Track baseline
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x2D, 0x30, 0x37));
    p.drawRect(0, height() / 2 - 8, width(), 16);

    // Markers
    if (m_model && m_durationMs > 0) {
        for (const auto& m : m_model->markers()) {
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
            p.drawRect(x1, height() / 2 - kBandHalfHeight, w, 2 * kBandHalfHeight);
        }
    }

    // Pending start handle (after `[` was pressed)
    if (m_pendingStartMs >= 0 && m_durationMs > 0) {
        const int x = msToX(m_pendingStartMs);
        p.setPen(QPen(QColor(0xFF, 0xC8, 0x4D), 2));
        p.drawLine(x, 4, x, height() - 4);
        p.setBrush(QColor(0xFF, 0xC8, 0x4D, 60));
        p.setPen(Qt::NoPen);
        p.drawRect(x, height() / 2 - kBandHalfHeight, msToX(m_positionMs) - x, 2 * kBandHalfHeight);
    }

    // Playhead
    if (m_durationMs > 0) {
        const int x = msToX(m_positionMs);
        p.setPen(QPen(QColor(0xF0, 0xF0, 0xF0), 2));
        p.drawLine(x, 0, x, height());
    }
}

void TimelineWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || m_durationMs <= 0) return;

    const Hit hit = hitTest(event->pos());
    if (hit.edge != Edge::None) {
        m_dragId   = hit.id;
        m_dragEdge = hit.edge;
        return;  // start an edge-drag; don't scrub
    }
    emit scrubbed(xToMs(event->pos().x()));
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

    if (event->buttons() & Qt::LeftButton) {
        emit scrubbed(xToMs(event->pos().x()));
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
    }
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
