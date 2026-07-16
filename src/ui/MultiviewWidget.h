#pragma once
#include <QImage>
#include <QWidget>

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

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QImage frame_;
    int inputCount_ = 1;
};

}  // namespace moo::ui
