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
#include <QComboBox>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStringList>

#include <vector>

#include "ui/EngineBridge.h"
#include "ui/MultiviewWidget.h"
#include "ui/ShowFile.h"

namespace moo::ui {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // showFile/initial may be null (no persistence, e.g. tests).
    MainWindow(EngineBridge& bridge, const QStringList& inputNames,
               ShowFile* showFile = nullptr,
               const ShowFile::State* initial = nullptr,
               QWidget* parent = nullptr);

    void saveShow();  // also called from aboutToQuit

private:
    void onState(int program, int preview, bool inTransition, bool ftb,
                 bool dsk1, bool dsk2);
    void onDskOptions(bool tie1, bool tie2, bool afv1, bool afv2);
    void onInputNames(const QStringList& refs);
    void pushTransition();
    void refreshBusReadouts();
    void refreshOutputFormatState();
    void refreshMediaControls();
    void refreshRecordingState();
    ShowFile::State collectState() const;

    EngineBridge& bridge_;
    ShowFile* showFile_ = nullptr;
    ShowFile::State baseState_;   // static cfg parts we don't edit live
    ShowFile::State lastSaved_;
    bool everSaved_ = false;
    MultiviewWidget* multiview_ = nullptr;
    QLabel* banner_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* clock_ = nullptr;
    QLabel* healthBadge_ = nullptr;
    QLabel* programReadout_ = nullptr;
    QLabel* previewReadout_ = nullptr;
    QPushButton* recordBtn_ = nullptr;
    QLabel* recordState_ = nullptr;
    QPushButton* cleanRecordBtn_ = nullptr;
    QLabel* cleanRecordState_ = nullptr;
    QComboBox* outputResolution_ = nullptr;
    QComboBox* outputFrameRate_ = nullptr;
    QLabel* outputFormatState_ = nullptr;
    std::vector<QPushButton*> pgmBtns_, pvwBtns_;
    std::vector<QComboBox*> inputPickers_;
    QPushButton* cutBtn_ = nullptr;
    QPushButton* autoBtn_ = nullptr;
    QPushButton* ftbBtn_ = nullptr;
    QSlider* tbar_ = nullptr;
    QComboBox* transType_ = nullptr;
    QSpinBox* transDur_ = nullptr;
    QPushButton* dskBtns_[kDskCount] = {nullptr, nullptr};
    QComboBox* dskSrc_[kDskCount] = {nullptr, nullptr};
    QSpinBox* dskFade_[kDskCount] = {nullptr, nullptr};
    QPushButton* dskTie_[kDskCount] = {nullptr, nullptr};
    QPushButton* dskAfv_[kDskCount] = {nullptr, nullptr};
    struct MediaRow {
        QLabel* name = nullptr;
        QLabel* time = nullptr;
        QPushButton* previous = nullptr;
        QPushButton* next = nullptr;
        QPushButton* play = nullptr;
        QPushButton* restart = nullptr;
        QPushButton* loop = nullptr;
    };
    std::vector<MediaRow> mediaRows_;
    QStringList inputNames_;
    VideoFormatDesc activeOutput_;
    int lastProgram_ = -1, lastPreview_ = -1;
    bool lastFtb_ = false;
    bool lastDskOn_[kDskCount] = {false, false};
    bool lastDskTie_[kDskCount] = {false, false};
    bool lastDskAfv_[kDskCount] = {false, false};
};

}  // namespace moo::ui
