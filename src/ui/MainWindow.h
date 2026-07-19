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
};

}  // namespace moo::ui
