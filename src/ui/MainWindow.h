#pragma once
#include <QLabel>
#include <QMainWindow>

#include "ui/EngineBridge.h"
#include "ui/MultiviewWidget.h"

namespace moo::ui {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(EngineBridge& bridge, const QStringList& inputNames,
               QWidget* parent = nullptr);

private:
    EngineBridge& bridge_;
    MultiviewWidget* multiview_ = nullptr;
    QLabel* status_ = nullptr;
};

}  // namespace moo::ui
