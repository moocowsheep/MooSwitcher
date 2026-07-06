#pragma once
#include <QComboBox>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>

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
    void onState(int program, int preview, bool inTransition, bool ftb);
    void onInputNames(const QStringList& refs);
    void pushTransition();
    ShowFile::State collectState() const;

    EngineBridge& bridge_;
    ShowFile* showFile_ = nullptr;
    ShowFile::State baseState_;   // static cfg parts we don't edit live
    ShowFile::State lastSaved_;
    bool everSaved_ = false;
    MultiviewWidget* multiview_ = nullptr;
    QLabel* banner_ = nullptr;
    QLabel* status_ = nullptr;
    std::vector<QPushButton*> pgmBtns_, pvwBtns_;
    QPushButton* ftbBtn_ = nullptr;
    QSlider* tbar_ = nullptr;
    QComboBox* transType_ = nullptr;
    QSpinBox* transDur_ = nullptr;
    int lastProgram_ = -1, lastPreview_ = -1;
    bool lastFtb_ = false;
};

}  // namespace moo::ui
