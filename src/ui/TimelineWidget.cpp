#include "TimelineWidget.h"

#include "core/Marker.h"
#include "core/MarkerModel.h"

#include <QMouseEvent>
#include <QPainter>
#include <QtMath>

namespace censorcut {

TimelineWidget::TimelineWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(48);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
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
            p.drawRect(x1, height() / 2 - 12, w, 24);
        }
    }

    // Pending start handle (after `[` was pressed)
    if (m_pendingStartMs >= 0 && m_durationMs > 0) {
        const int x = msToX(m_pendingStartMs);
        p.setPen(QPen(QColor(0xFF, 0xC8, 0x4D), 2));
        p.drawLine(x, 4, x, height() - 4);
        p.setBrush(QColor(0xFF, 0xC8, 0x4D, 60));
        p.setPen(Qt::NoPen);
        p.drawRect(x, height() / 2 - 12, msToX(m_positionMs) - x, 24);
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
    if (event->button() == Qt::LeftButton && m_durationMs > 0) {
        emit scrubbed(xToMs(event->pos().x()));
    }
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* event)
{
    if ((event->buttons() & Qt::LeftButton) && m_durationMs > 0) {
        emit scrubbed(xToMs(event->pos().x()));
    }
}

} // namespace censorcut
