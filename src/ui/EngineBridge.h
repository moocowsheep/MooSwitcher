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

    // Audio mixer controls: straight to the mixer atomics (thread-safe).
    void setAudioGain(int input, float linearGain);
    void setAudioMute(int input, bool on);
    void setAudioSolo(int input, bool on);
    void setAudioDelayMs(int input, int ms);
    void setMasterDelayMs(int ms);

    // Source picker: srt:// refs become SRT inputs, anything else is an NDI
    // name substring. Takes effect at the next render tick.
    void replaceInput(int input, QString ref);

public:
    bool audioAvailable() const { return engine_.audio() != nullptr; }
    int audioInputDelayMs(int input) const;
    int masterDelayMs() const;
    float audioGain(int input) const;
    bool audioMute(int input) const;
    bool audioSolo(int input) const;
    QStringList ndiSourceNames() const;
    QString inputRef(int input) const;
    int inputCount() const { return engine_.inputCount(); }

signals:
    void multiviewFrame(QImage frame);
    void statusText(QString text);
    void stateChanged(int program, int preview, bool inTransition, bool ftb);
    void audioLevels(QList<float> lr);  // per input L,R ... then master L,R
    void inputNamesChanged(QStringList refs);
    void healthChanged(QStringList problems);  // empty = all good

private:
    void poll();

    Engine& engine_;
    QTimer timer_;
    std::vector<uint8_t> buf_;
    uint64_t seq_ = 0;
    QStringList lastRefs_;
    QStringList lastProblems_;
};

}  // namespace moo::ui
