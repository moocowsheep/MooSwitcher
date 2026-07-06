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

namespace moo::ui {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(EngineBridge& bridge, const QStringList& inputNames,
               QWidget* parent = nullptr);

private:
    void onState(int program, int preview, bool inTransition, bool ftb);
    void pushTransition();

    EngineBridge& bridge_;
    MultiviewWidget* multiview_ = nullptr;
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
