#include "ui/MainWindow.h"

#include <QHBoxLayout>
#include <QKeySequence>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>

namespace moo::ui {

namespace {
QPushButton* busButton(const QString& text, const QString& color) {
    auto* b = new QPushButton(text);
    b->setMinimumSize(72, 48);
    b->setStyleSheet(QStringLiteral("QPushButton { font-weight: bold; }"
                                    "QPushButton:pressed { background: %1; }")
                         .arg(color));
    return b;
}
}  // namespace

MainWindow::MainWindow(EngineBridge& bridge, const QStringList& inputNames,
                       QWidget* parent)
    : QMainWindow(parent), bridge_(bridge) {
    setWindowTitle(QStringLiteral("MooSwitcher"));

    auto* central = new QWidget;
    auto* root = new QVBoxLayout(central);

    multiview_ = new MultiviewWidget;
    root->addWidget(multiview_, 1);
    connect(&bridge_, &EngineBridge::multiviewFrame, multiview_,
            &MultiviewWidget::setFrame);

    auto makeBusRow = [&](const QString& label, const QString& color, auto slot) {
        auto* row = new QHBoxLayout;
        auto* tag = new QLabel(label);
        tag->setMinimumWidth(48);
        tag->setStyleSheet(QStringLiteral("font-weight: bold;"));
        row->addWidget(tag);
        for (int i = 0; i < inputNames.size(); ++i) {
            auto* b = busButton(QStringLiteral("%1\n%2").arg(i + 1).arg(inputNames[i]),
                                color);
            connect(b, &QPushButton::clicked, &bridge_, [&bridge = bridge_, i, slot] {
                (bridge.*slot)(i);
            });
            row->addWidget(b);
        }
        row->addStretch(1);
        return row;
    };
    root->addLayout(makeBusRow(QStringLiteral("PGM"), QStringLiteral("#c02020"),
                               &EngineBridge::setProgram));
    root->addLayout(makeBusRow(QStringLiteral("PVW"), QStringLiteral("#209020"),
                               &EngineBridge::setPreview));

    auto* transRow = new QHBoxLayout;
    auto* cutBtn = new QPushButton(QStringLiteral("CUT"));
    cutBtn->setMinimumSize(120, 56);
    cutBtn->setStyleSheet(QStringLiteral("font-weight: bold; font-size: 16px;"));
    connect(cutBtn, &QPushButton::clicked, &bridge_, &EngineBridge::cut);
    transRow->addWidget(cutBtn);
    transRow->addStretch(1);
    root->addLayout(transRow);

    status_ = new QLabel;
    status_->setStyleSheet(QStringLiteral("font-family: monospace;"));
    root->addWidget(status_);
    connect(&bridge_, &EngineBridge::statusText, status_, &QLabel::setText);

    new QShortcut(QKeySequence(Qt::Key_Space), this, [this] { bridge_.cut(); });

    setCentralWidget(central);
    resize(1100, 760);
}

}  // namespace moo::ui
