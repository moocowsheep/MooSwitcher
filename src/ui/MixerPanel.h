#pragma once
#include <QWidget>

#include <vector>

#include "ui/EngineBridge.h"

namespace moo::ui {

// Stereo peak meter: -60..0 dBFS, green/yellow/red zones, peak-hold with
// decay. Fed linear peak-since-last-poll values at the bridge's 30 Hz.
class MeterWidget : public QWidget {
    Q_OBJECT
public:
    explicit MeterWidget(QWidget* parent = nullptr);
    void setLevels(float l, float r);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    float dispL_ = 0.f, dispR_ = 0.f;  // decayed display peaks (linear)
    float holdL_ = 0.f, holdR_ = 0.f;  // hold marks
    int holdAgeL_ = 0, holdAgeR_ = 0;
};

// The audio mixer surface: one strip per input (meter, fader, mute/solo,
// delay trim) plus a master strip (meter + the A/V calibration delay).
// Controls poke the engine's mixer atomics through the bridge; meters
// update from the poll signal.
class MixerPanel : public QWidget {
    Q_OBJECT
public:
    MixerPanel(EngineBridge& bridge, const QStringList& names,
               QWidget* parent = nullptr);

public slots:
    void onLevels(QList<float> lr);  // [in0 L,R, in1 L,R, ..., master L,R]

private:
    std::vector<MeterWidget*> meters_;
    MeterWidget* masterMeter_ = nullptr;
};

}  // namespace moo::ui
