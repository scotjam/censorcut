#include "VideoSurface.h"

#include <QPalette>

namespace censorcut {

VideoSurface::VideoSurface(QWidget* parent)
    : QWidget(parent)
{
    // The native window must exist before libVLC can attach.
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_PaintOnScreen);  // libVLC paints, not Qt
    setAutoFillBackground(false);

    // The native HWND would otherwise grab focus on click and swallow key
    // events (spacebar play/pause, arrow seeks, [/]) — all of those go via
    // MainWindow's keyPressEvent which only runs when MainWindow's widget
    // tree owns focus.
    setFocusPolicy(Qt::NoFocus);

    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    setPalette(pal);
    setMinimumSize(640, 360);
}

} // namespace censorcut
