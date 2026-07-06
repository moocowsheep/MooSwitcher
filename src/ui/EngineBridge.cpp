#include "ui/EngineBridge.h"

#include "core/Log.h"

namespace moo::ui {

namespace {
audio::InputChannel* chan(Engine& e, int i) {
    auto* a = e.audio();
    return (a && i >= 0 && i < a->inputCount()) ? &a->channel(i) : nullptr;
}
}  // namespace

EngineBridge::EngineBridge(Engine& engine, QObject* parent)
    : QObject(parent), engine_(engine) {
    connect(&timer_, &QTimer::timeout, this, &EngineBridge::poll);
    timer_.start(33);
}

void EngineBridge::setAudioGain(int input, float linearGain) {
    if (auto* c = chan(engine_, input))
        c->gain.store(linearGain, std::memory_order_relaxed);
}

void EngineBridge::setAudioMute(int input, bool on) {
    if (auto* c = chan(engine_, input))
        c->mute.store(on, std::memory_order_relaxed);
}

void EngineBridge::setAudioSolo(int input, bool on) {
    if (auto* c = chan(engine_, input))
        c->solo.store(on, std::memory_order_relaxed);
}

void EngineBridge::setAudioDelayMs(int input, int ms) {
    if (auto* c = chan(engine_, input))
        c->delayMs.store(ms, std::memory_order_relaxed);
}

void EngineBridge::setMasterDelayMs(int ms) {
    if (auto* a = engine_.audio())
        a->masterDelayMs.store(ms, std::memory_order_relaxed);
}

int EngineBridge::audioInputDelayMs(int input) const {
    auto* c = chan(engine_, input);
    return c ? c->delayMs.load(std::memory_order_relaxed) : 0;
}

int EngineBridge::masterDelayMs() const {
    auto* a = engine_.audio();
    return a ? a->masterDelayMs.load(std::memory_order_relaxed) : 0;
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

    if (auto* a = engine_.audio()) {
        QList<float> lv;
        lv.reserve(a->inputCount() * 2 + 2);
        for (int i = 0; i < a->inputCount(); ++i) {
            lv << a->channel(i).peakL.exchange(0.f, std::memory_order_relaxed)
               << a->channel(i).peakR.exchange(0.f, std::memory_order_relaxed);
        }
        lv << a->masterPeakL.exchange(0.f, std::memory_order_relaxed)
           << a->masterPeakR.exchange(0.f, std::memory_order_relaxed);
        emit audioLevels(lv);
    }

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
