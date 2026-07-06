#include "ui/MultiviewWidget.h"

#include <QPainter>

namespace moo::ui {

MultiviewWidget::MultiviewWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(640, 360);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    setAutoFillBackground(true);
    setPalette(pal);
}

void MultiviewWidget::setFrame(QImage img) {
    frame_ = std::move(img);
    update();
}

void MultiviewWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (frame_.isNull()) return;
    QSize target = frame_.size().scaled(size(), Qt::KeepAspectRatio);
    QRect dst(QPoint((width() - target.width()) / 2, (height() - target.height()) / 2),
              target);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(dst, frame_);
}

}  // namespace moo::ui
