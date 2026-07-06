#include "ui/EngineBridge.h"

namespace moo::ui {

EngineBridge::EngineBridge(Engine& engine, QObject* parent)
    : QObject(parent), engine_(engine) {
    connect(&timer_, &QTimer::timeout, this, &EngineBridge::poll);
    timer_.start(33);
}

void EngineBridge::poll() {
    int w = 0, h = 0;
    if (engine_.copyMultiview(buf_, seq_, w, h)) {
        // Deep copy: buf_ is reused on the next poll.
        QImage img(buf_.data(), w, h, w * 4, QImage::Format_RGBA8888);
        emit multiviewFrame(img.copy());
    }

    const auto st = engine_.uiState();
    emit stateChanged(st.program, st.preview, st.inTransition, st.ftbEngaged);

    QString status = QStringLiteral("ticks %1  skips %2  ndi-out %3")
                         .arg(engine_.renderedTicks())
                         .arg(engine_.skippedTicks())
                         .arg(engine_.ndiOutFrames());
    for (int i = 0; i < engine_.inputCount(); ++i) {
        const auto s = engine_.inputStatus(i);
        status += QStringLiteral("   in%1: %2 %3x%4 f=%5 d=%6")
                      .arg(i)
                      .arg(s.connected ? QStringLiteral("up") : QStringLiteral("down"))
                      .arg(s.desc.width)
                      .arg(s.desc.height)
                      .arg(s.frames)
                      .arg(s.drops);
    }
    emit statusText(status);
}

}  // namespace moo::ui
