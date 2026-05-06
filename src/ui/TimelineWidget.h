#pragma once

#include <QUuid>
#include <QWidget>

namespace censorcut {

class MarkerModel;

/// A horizontal timeline showing playhead, pending start-marker (after `[`),
/// and all confirmed/pending markers as colored bands.
/// Click to seek; drag a marker's left or right edge to adjust its time;
/// right-click a marker for a delete/confirm/reject menu.
/// Ctrl+wheel zooms (centred on the cursor's time); Shift+wheel pans.
/// '0' resets the zoom to fit the whole video.
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

    /// Reset the visible window to the full duration.
    void resetZoom();

signals:
    void scrubbed(qint64 ms);    // user clicked or dragged on empty timeline
    void scrubBegan();           // mouse pressed on the scrubber strip
    void scrubEnded();           // mouse released after a scrub

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    enum class Edge { None, Start, End };
    struct Hit { QUuid id; Edge edge = Edge::None; bool body = false; };

    qint64 xToMs(int x) const;
    int    msToX(qint64 ms) const;
    QRect  markerBandRect() const;
    QRect  scrubberRect() const;
    Hit    hitTest(const QPoint& pos) const;
    void   showMarkerMenu(const QUuid& id, const QPoint& globalPos);
    void   zoomBy(double factor, int anchorX);
    void   panBy(qint64 deltaMs);
    void   clampView();

    MarkerModel* m_model = nullptr;
    qint64 m_durationMs = 0;
    qint64 m_positionMs = 0;
    qint64 m_pendingStartMs = -1;

    // View window in ms: [m_viewStartMs, m_viewEndMs). When equal to
    // [0, m_durationMs) the whole video is visible.
    qint64 m_viewStartMs = 0;
    qint64 m_viewEndMs   = 0;

    // Drag state for resizing a marker by its edge.
    QUuid m_dragId;
    Edge  m_dragEdge = Edge::None;
};

} // namespace censorcut
