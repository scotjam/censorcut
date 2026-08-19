#pragma once

#include <QWidget>

namespace censorcut {

/// A QWidget that owns a native window suitable for libVLC to render into.
/// libVLC will paint directly onto this surface — Qt should not paint over it.
class VideoSurface : public QWidget {
    Q_OBJECT
public:
    explicit VideoSurface(QWidget* parent = nullptr);

protected:
#if !defined(Q_OS_WIN)
    // X11 path only: libVLC paints directly into this window, Qt must not.
    // On Windows the widget is just a backdrop behind libVLC's own child
    // HWND, and Qt needs its paint machinery to fill the background black.
    QPaintEngine* paintEngine() const override { return nullptr; }
#endif
    void mousePressEvent(QMouseEvent* event) override;
};

} // namespace censorcut
