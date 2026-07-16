#include "ui/MultiviewWidget.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>

namespace moo::ui {

MultiviewWidget::MultiviewWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(640, 280);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

QSize MultiviewWidget::sizeHint() const { return {960, 500}; }

void MultiviewWidget::setFrame(QImage img) {
    frame_ = std::move(img);
    update();
}

void MultiviewWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(5, 7, 9));

    // A restrained alignment grid keeps the monitor intentional while the
    // engine is starting, and remains visible only in letterboxed margins.
    painter.setPen(QPen(QColor(26, 32, 39), 1));
    constexpr int kGrid = 48;
    for (int x = 0; x < width(); x += kGrid) painter.drawLine(x, 0, x, height());
    for (int y = 0; y < height(); y += kGrid) painter.drawLine(0, y, width(), y);

    const QRectF stage = rect().adjusted(5, 5, -5, -5);
    painter.setPen(QPen(QColor(47, 57, 68), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(stage, 4, 4);

    if (frame_.isNull()) {
        painter.setPen(QColor(93, 106, 120));
        QFont title = painter.font();
        title.setPointSize(12);
        title.setBold(true);
        title.setLetterSpacing(QFont::AbsoluteSpacing, 2.0);
        painter.setFont(title);
        painter.drawText(rect().adjusted(0, -14, 0, 0), Qt::AlignCenter,
                         QStringLiteral("WAITING FOR MULTIVIEW"));
        QFont detail = painter.font();
        detail.setPointSize(9);
        detail.setBold(false);
        detail.setLetterSpacing(QFont::AbsoluteSpacing, 0.2);
        painter.setFont(detail);
        painter.setPen(QColor(65, 76, 88));
        painter.drawText(rect().adjusted(0, 23, 0, 0), Qt::AlignCenter,
                         QStringLiteral("Video surfaces will appear when the engine publishes a frame"));
        return;
    }

    // The engine publishes one image whose left half is the input matrix and
    // right half is PROGRAM/PREVIEW. Present those banks independently so the
    // inputs anchor to the left edge and the output monitors anchor right;
    // surplus width becomes useful visual separation instead of side bars.
    const int split = frame_.width() / 2;
    constexpr int kColumnGap = 0;
    const int inputWidth = split - kColumnGap / 2;
    const int inputColumns = std::clamp(inputCount_, 1, 6);
    const int inputRows = std::max(1, (inputCount_ + inputColumns - 1) /
                                          inputColumns);
    const int inputCellWidth = (inputWidth / inputColumns) & ~1;
    constexpr int kInputLabelHeight = 24;  // compositor label row height
    const int inputVideoHeight =
        std::max(2, (inputCellWidth * 9 / 16) & ~1);
    const int naturalInputHeight = inputVideoHeight + kInputLabelHeight;
    const int inputCellHeight = std::min(
        naturalInputHeight,
        std::max(kInputLabelHeight, (frame_.height() / inputRows) & ~1));
    const int inputHeight =
        std::min(frame_.height(), inputCellHeight * inputRows);
    const QRect inputSource(0, 0, split, inputHeight);
    const QRect outputSource(split, 0, frame_.width() - split, frame_.height());
    const QSize bankBounds(std::max(1, (width() - 22) / 2),
                           std::max(1, height() - 12));
    const QSize outputSize = outputSource.size().scaled(bankBounds,
                                                         Qt::KeepAspectRatio);
    const QRect outputDestination(
        QPoint(width() - outputSize.width() - 6,
               (height() - outputSize.height()) / 2),
        outputSize);
    const QSize inputBounds(std::max(1, outputDestination.left() - 6),
                            std::max(1, height() - 12));
    const QSize inputSize = inputSource.size().scaled(inputBounds,
                                                       Qt::KeepAspectRatio);
    const QRect inputDestination(QPoint(6, 6), inputSize);

    const auto drawBank = [&](const QRect& destination, const QRect& source) {
        QPainterPath clip;
        clip.addRoundedRect(QRectF(destination), 3, 3);
        painter.save();
        painter.setClipPath(clip);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(destination, frame_, source);
        painter.restore();

        painter.setPen(QPen(QColor(75, 87, 99), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(
            QRectF(destination).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    };
    drawBank(inputDestination, inputSource);
    drawBank(outputDestination, outputSource);
}

}  // namespace moo::ui
