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

#pragma once
#include <QLabel>
#include <QPushButton>
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

// The audio mixer surface: one strip per input (source-picker header,
// meter, fader, mute/solo, delay trim) plus a master strip (meter + the
// A/V calibration delay). Controls poke the engine's mixer atomics through
// the bridge; meters update from the poll signal.
class MixerPanel : public QWidget {
    Q_OBJECT
public:
    MixerPanel(EngineBridge& bridge, const QStringList& names,
               QWidget* parent = nullptr);

public slots:
    void onLevels(QList<float> lr);  // [in0 L,R, in1 L,R, ..., master L,R]
    void onInputNames(QStringList refs);

private:
    EngineBridge& bridge_;
    std::vector<MeterWidget*> meters_;
    std::vector<QPushButton*> nameBtns_;
    std::vector<QLabel*> trimLabels_;  // frame-sync auto trim readouts
    MeterWidget* masterMeter_ = nullptr;
};

}  // namespace moo::ui
