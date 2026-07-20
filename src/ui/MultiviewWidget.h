/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

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
