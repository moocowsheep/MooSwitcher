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
 * runtime, and distribute the combined work. See LICENSE.md for the full
 * exception text. */

#pragma once
#include <QImage>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QEvent;
class QMouseEvent;

namespace moo::ui {

// CPU presenter for the engine's multiview readback. This is the presenter
// seam: a zero-copy QRhi import of the engine's VkImage drops in here later
// without touching anything else.
class MultiviewWidget : public QWidget {
    Q_OBJECT
public:
    explicit MultiviewWidget(QWidget* parent = nullptr);

    void setFrame(QImage img);  // takes ownership (already deep-copied)
    void setInputCount(int count) { inputCount_ = count; }
    void setInputNames(QStringList names) {
        inputNames_ = std::move(names);
        update();
    }

    QSize sizeHint() const override;

signals:
    void previewSourceRequested(int source);
    void programSourceRequested(int source);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    int sourceAt(const QPointF& position) const;

    QImage frame_;
    QStringList inputNames_;
    QVector<QRectF> inputHitRects_;
    int inputCount_ = 1;
    int hoveredSource_ = -1;
};

}  // namespace moo::ui
