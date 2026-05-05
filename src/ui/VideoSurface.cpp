#include "VideoSurface.h"

#include <QMouseEvent>
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

void VideoSurface::mousePressEvent(QMouseEvent* event)
{
    // We don't accept focus ourselves (the native HWND would swallow keys),
    // but a click on the video is the user's natural "back to editing"
    // gesture — hand focus to the top-level window so transport keys
    // (Space, arrows, J/K/L, etc.) reach MainWindow::keyPressEvent again.
    if (auto* w = window()) w->setFocus(Qt::MouseFocusReason);
    QWidget::mousePressEvent(event);
}

} // namespace censorcut
