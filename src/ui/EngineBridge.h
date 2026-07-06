#pragma once
#include <QImage>
#include <QObject>
#include <QTimer>

#include <vector>

#include "engine/Engine.h"

namespace moo::ui {

// GUI-side adapter: commands in (queued to the engine), multiview frames and
// state out (30 Hz poll). No engine callbacks ever run on the GUI thread.
class EngineBridge : public QObject {
    Q_OBJECT
public:
    explicit EngineBridge(Engine& engine, QObject* parent = nullptr);

public slots:
    void setProgram(int src) { engine_.post({Command::Type::SetProgram, src, 0, 0.f}); }
    void setPreview(int src) { engine_.post({Command::Type::SetPreview, src, 0, 0.f}); }
    void cut() { engine_.post({Command::Type::Cut, 0, 0, 0.f}); }
    void autoTrans() { engine_.post({Command::Type::Auto, 0, 0, 0.f}); }
    void fadeToBlack() { engine_.post({Command::Type::FadeToBlack, 0, 0, 0.f}); }
    void tbarBegin() { engine_.post({Command::Type::TbarBegin, 0, 0, 0.f}); }
    void tbarSet(float pos) { engine_.post({Command::Type::TbarSet, 0, 0, pos}); }
    void tbarEnd() { engine_.post({Command::Type::TbarEnd, 0, 0, 0.f}); }
    void setTransition(int type, int durationTicks, float softness) {
        engine_.post({Command::Type::SetTransition, type, durationTicks, softness});
    }

signals:
    void multiviewFrame(QImage frame);
    void statusText(QString text);
    void stateChanged(int program, int preview, bool inTransition, bool ftb);

private:
    void poll();

    Engine& engine_;
    QTimer timer_;
    std::vector<uint8_t> buf_;
    uint64_t seq_ = 0;
};

}  // namespace moo::ui
