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
///
/// Three horizontal bands, each with its own coordinate space:
///   - mini-map (top)     — absolute over the whole film; click to jump
///                          anywhere without leaving the current zoom.
///   - marker band (mid)  — zoom-relative; where markers live.
///   - scrubber (bottom)  — absolute over the whole film; the progress bar.
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

    /// Which coordinate space an in-progress scrub drag is using. The marker
    /// band is zoom-relative; the scrubber strip and the mini-map are always
    /// absolute over the whole duration, so a drag has to remember which
    /// mapping it started in.
    enum class ScrubSpace { None, View, Scrubber, MiniMap };

    // View-relative mapping — used by the marker band, which is what zooming
    // is for. Spans [m_viewStartMs, m_viewEndMs) across the widget's width.
    qint64 xToMs(int x) const;
    int    msToX(qint64 ms) const;

    // Absolute mapping over [0, m_durationMs] along the scrubber track. Never
    // affected by zoom, so the handle always shows position within the film.
    qint64 scrubXToMs(int x) const;
    int    msToScrubX(qint64 ms) const;

    // Absolute mapping across the full widget width — the mini-map.
    qint64 miniMapXToMs(int x) const;

    QRect  miniMapRect() const;      // clickable band, taller than the drawn strip
    QRect  markerBandRect() const;
    QRect  scrubberRect() const;
    QRect  scrubTrackRect() const;   // scrubber inset so the handle never clips
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

    // Drag state for a scrub. Non-None between scrubBegan and scrubEnded.
    ScrubSpace m_scrubSpace = ScrubSpace::None;
};

} // namespace censorcut
