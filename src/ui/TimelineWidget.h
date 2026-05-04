#pragma once

#include <QWidget>

namespace censorcut {

class MarkerModel;

/// A horizontal timeline showing playhead, pending start-marker (after `[`),
/// and all confirmed/pending markers as colored bands.
/// Click to seek.
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
    void scrubbed(qint64 ms);  // user clicked on the timeline

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    qint64 xToMs(int x) const;
    int    msToX(qint64 ms) const;

    MarkerModel* m_model = nullptr;
    qint64 m_durationMs = 0;
    qint64 m_positionMs = 0;
    qint64 m_pendingStartMs = -1;
};

} // namespace censorcut
