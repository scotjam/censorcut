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
    QPaintEngine* paintEngine() const override { return nullptr; }
};

} // namespace censorcut
