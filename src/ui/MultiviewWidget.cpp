/* MooSwitcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * Additional permission under GNU GPL version 3 section 7: you may link
 * MooSwitcher against the proprietary NDI SDK, the NVIDIA CUDA / Video
 * Codec SDK runtime (CUDA, NVENC, NVDEC), and the OMT (libomt / libvmx)
 * runtime, and distribute the combined work. See LICENSE-EXCEPTION.md for
 * the full exception text. */

#include "ui/MultiviewWidget.h"

#include <QFontMetrics>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

namespace moo::ui {

MultiviewWidget::MultiviewWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(640, 280);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
}

QSize MultiviewWidget::sizeHint() const { return {960, 500}; }

void MultiviewWidget::setFrame(QImage img) {
    frame_ = std::move(img);
    update();
}

void MultiviewWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QLinearGradient surface(0, 0, 0, height());
    surface.setColorAt(0.0, QColor(13, 14, 16));
    surface.setColorAt(1.0, QColor(7, 8, 9));
    painter.fillRect(rect(), surface);

    // A restrained alignment grid keeps the monitor intentional while the
    // engine is starting, and remains visible only in letterboxed margins.
    painter.setPen(QPen(QColor(25, 28, 31), 1));
    constexpr int kGrid = 48;
    for (int x = 0; x < width(); x += kGrid) painter.drawLine(x, 0, x, height());
    for (int y = 0; y < height(); y += kGrid) painter.drawLine(0, y, width(), y);

    const QRectF stage = rect().adjusted(4, 4, -4, -4);
    painter.setPen(QPen(QColor(46, 50, 55), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(stage, 4, 4);

    if (frame_.isNull()) {
        inputHitRects_.clear();
        painter.setPen(QColor(123, 130, 137));
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
        painter.setPen(QColor(86, 92, 99));
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
    // Mirrors the compositor's grid: one row up to 6 inputs, at most three
    // rows beyond (21 inputs = 7 x 3).
    const int inputColumns =
        inputCount_ <= 6 ? std::max(inputCount_, 1) : (inputCount_ + 2) / 3;
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
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 120));
        painter.drawRoundedRect(QRectF(destination).translated(0, 3), 4, 4);

        QPainterPath clip;
        clip.addRoundedRect(QRectF(destination), 3, 3);
        painter.save();
        painter.setClipPath(clip);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(destination, frame_, source);
        painter.restore();

        painter.setPen(QPen(QColor(73, 79, 86), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(
            QRectF(destination).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    };
    drawBank(inputDestination, inputSource);
    drawBank(outputDestination, outputSource);

    // Draw all monitor labels after the banks reach their final size. Text in
    // the GPU image would be resampled along with the thumbnails and become
    // soft; this final-resolution overlay gives inputs and outputs one shared
    // Qt font treatment.
    const qreal scaleX = qreal(inputDestination.width()) / inputSource.width();
    const qreal scaleY = qreal(inputDestination.height()) / inputSource.height();
    QFont labelFont(QStringLiteral("Noto Sans"));
    labelFont.setPixelSize(11);
    labelFont.setWeight(QFont::DemiBold);
    const QFontMetrics metrics(labelFont);

    painter.save();
    painter.setClipRect(inputDestination);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setFont(labelFont);
    inputHitRects_.clear();
    inputHitRects_.reserve(inputCount_);
    for (int i = 0; i < inputCount_; ++i) {
        const int col = i % inputColumns;
        const int row = i / inputColumns;
        const int sourceX = col * inputCellWidth;
        const int sourceY = row * inputCellHeight;
        const int sourceW = col == inputColumns - 1
                                ? inputWidth - sourceX
                                : inputCellWidth;
        const int sourceH =
            std::min(inputCellHeight, frame_.height() - sourceY);
        inputHitRects_.push_back(QRectF(
            inputDestination.left() + sourceX * scaleX,
            inputDestination.top() + sourceY * scaleY,
            sourceW * scaleX, sourceH * scaleY));
        const QRectF labelRect(
            inputDestination.left() + sourceX * scaleX,
            inputDestination.top() +
                (sourceY + sourceH - kInputLabelHeight) * scaleY,
            sourceW * scaleX, kInputLabelHeight * scaleY);
        painter.fillRect(labelRect, QColor(10, 11, 13, 240));
        painter.fillRect(QRectF(labelRect.left(), labelRect.top(),
                                labelRect.width(), 1),
                         QColor(43, 84, 99));
        painter.fillRect(QRectF(labelRect.left(), labelRect.top(), 3,
                                labelRect.height()),
                         QColor(47, 201, 242));
        const QRectF textRect = labelRect.adjusted(7, 0, -5, 0);
        QString name = i < inputNames_.size()
                           ? inputNames_[i]
                           : QStringLiteral("INPUT %1").arg(i + 1);
        name.replace(QLatin1Char('_'), QLatin1Char(' '));
        const QString text =
            QStringLiteral("%1  %2")
                .arg(i + 1, 2, 10, QLatin1Char('0'))
                .arg(name.toUpper());
        painter.setPen(QColor(222, 227, 232));
        painter.drawText(
            textRect, Qt::AlignLeft | Qt::AlignVCenter,
            metrics.elidedText(text, Qt::ElideRight, int(textRect.width())));
    }
    if (hoveredSource_ >= 0 && hoveredSource_ < inputHitRects_.size()) {
        painter.setPen(QPen(QColor(63, 214, 247), 2));
        painter.setBrush(QColor(47, 201, 242, 22));
        painter.drawRoundedRect(
            inputHitRects_[hoveredSource_].adjusted(2, 2, -2, -2), 3, 3);
    }
    painter.restore();

    // PROGRAM and PREVIEW occupy the two stacked cells in the output bank.
    // Their label strips are generated by the compositor at the bottom of
    // each cell, just like the input strips above.
    constexpr int kOutputGap = 4;
    const int outputCellHeight = ((frame_.height() - kOutputGap) / 2) & ~1;
    const qreal outputScaleY =
        qreal(outputDestination.height()) / outputSource.height();
    auto drawOutputLabel = [&](const QString& text, int sourceBottom,
                               const QColor& accent) {
        constexpr qreal kLabelHeight = 22.0;
        const qreal bottom =
            outputDestination.top() + sourceBottom * outputScaleY;
        const QRectF labelRect(
            outputDestination.left(), bottom - kLabelHeight,
            outputDestination.width(), kLabelHeight);
        painter.fillRect(labelRect, QColor(8, 9, 11, 242));
        painter.fillRect(QRectF(labelRect.left(), labelRect.top(), 4,
                                labelRect.height()),
                         accent);
        painter.fillRect(QRectF(labelRect.left(), labelRect.top(),
                                labelRect.width(), 1),
                         accent.darker(135));
        const QRectF textRect = labelRect.adjusted(9, 0, -5, 0);
        painter.setPen(accent.lighter(130));
        painter.drawText(
            textRect, Qt::AlignLeft | Qt::AlignVCenter,
            metrics.elidedText(text, Qt::ElideRight, int(textRect.width())));
    };

    painter.save();
    painter.setClipRect(outputDestination);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setFont(labelFont);
    drawOutputLabel(QStringLiteral("PROGRAM"), outputCellHeight,
                    QColor(240, 52, 64));
    drawOutputLabel(QStringLiteral("PREVIEW"), frame_.height(),
                    QColor(46, 197, 112));
    painter.restore();
}

int MultiviewWidget::sourceAt(const QPointF& position) const {
    for (int i = 0; i < inputHitRects_.size(); ++i)
        if (inputHitRects_[i].contains(position)) return i;
    return -1;
}

void MultiviewWidget::mousePressEvent(QMouseEvent* event) {
    const int source = sourceAt(event->position());
    if (source < 0) {
        QWidget::mousePressEvent(event);
        return;
    }
    if (event->button() == Qt::LeftButton) {
        emit previewSourceRequested(source);
        event->accept();
    } else if (event->button() == Qt::RightButton) {
        emit programSourceRequested(source);
        event->accept();
    } else {
        QWidget::mousePressEvent(event);
    }
}

void MultiviewWidget::mouseMoveEvent(QMouseEvent* event) {
    const int hovered = sourceAt(event->position());
    if (hovered != hoveredSource_) {
        hoveredSource_ = hovered;
        update();
    }
    setCursor(hovered >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    QWidget::mouseMoveEvent(event);
}

void MultiviewWidget::leaveEvent(QEvent* event) {
    if (hoveredSource_ >= 0) {
        hoveredSource_ = -1;
        update();
    }
    unsetCursor();
    QWidget::leaveEvent(event);
}

}  // namespace moo::ui
