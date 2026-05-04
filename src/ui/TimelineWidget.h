#pragma once

#include <QUuid>
#include <QWidget>

namespace censorcut {

class MarkerModel;

/// A horizontal timeline showing playhead, pending start-marker (after `[`),
/// and all confirmed/pending markers as colored bands.
/// Click to seek; drag a marker's left or right edge to adjust its time;
/// right-click a marker for a delete/confirm/reject menu.
class TimelineWidget : public QWidget {
    Q_OBJECT
public:
    explicit TimelineWidget(QWidget* parent = nullptr);

    void setModel(MarkerModel* model);
    void setDurationMs(qint64 duration);
    void setPositionMs(qint64 position);

    /// The "in-progress" cut start (set by `[`), shown as a vertical handle
    /// until `]` finalizes it. Pass -1 to clear.
    void setPendingCutStartMs(qint64 ms);

signals:
    void scrubbed(qint64 ms);  // user clicked or dragged on empty timeline

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    enum class Edge { None, Start, End };
    struct Hit { QUuid id; Edge edge = Edge::None; bool body = false; };

    qint64 xToMs(int x) const;
    int    msToX(qint64 ms) const;
    Hit    hitTest(const QPoint& pos) const;
    void   showMarkerMenu(const QUuid& id, const QPoint& globalPos);

    MarkerModel* m_model = nullptr;
    qint64 m_durationMs = 0;
    qint64 m_positionMs = 0;
    qint64 m_pendingStartMs = -1;

    // Drag state for resizing a marker by its edge.
    QUuid m_dragId;
    Edge  m_dragEdge = Edge::None;
};

} // namespace censorcut
