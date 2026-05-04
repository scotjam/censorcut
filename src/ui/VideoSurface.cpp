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

    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    setPalette(pal);
    setMinimumSize(640, 360);
}

} // namespace censorcut
