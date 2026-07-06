#pragma once
#include <QImage>
#include <QObject>
#include <QTimer>

#include <vector>

#include "engine/Engine.h"

namespace moo::ui {

// GUI-side adapter: commands in (queued to the engine), multiview frames and
// stats out (30 Hz poll). No engine callbacks ever run on the GUI thread.
class EngineBridge : public QObject {
    Q_OBJECT
public:
    explicit EngineBridge(Engine& engine, QObject* parent = nullptr);

public slots:
    void setProgram(int src) { engine_.post({Command::Type::SetProgram, src}); }
    void setPreview(int src) { engine_.post({Command::Type::SetPreview, src}); }
    void cut() { engine_.post({Command::Type::Cut, 0}); }

signals:
    void multiviewFrame(QImage frame);
    void statusText(QString text);

private:
    void poll();

    Engine& engine_;
    QTimer timer_;
    std::vector<uint8_t> buf_;
    uint64_t seq_ = 0;
};

}  // namespace moo::ui
