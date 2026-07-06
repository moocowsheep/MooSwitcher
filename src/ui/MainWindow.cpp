#include "ui/MainWindow.h"

#include <QHBoxLayout>
#include <QKeySequence>
#include <QShortcut>
#include <QVBoxLayout>

namespace moo::ui {

namespace {
const char* kBtnBase =
    "QPushButton { font-weight: bold; min-width: 72px; min-height: 44px; }";
const char* kBtnPgm =
    "QPushButton { font-weight: bold; min-width: 72px; min-height: 44px; "
    "background: #b01818; color: white; }";
const char* kBtnPvw =
    "QPushButton { font-weight: bold; min-width: 72px; min-height: 44px; "
    "background: #1d8a24; color: white; }";
const char* kBtnFtbOn =
    "QPushButton { font-weight: bold; min-width: 72px; min-height: 44px; "
    "background: #202020; color: #ff4040; }";
}  // namespace

MainWindow::MainWindow(EngineBridge& bridge, const QStringList& inputNames,
                       QWidget* parent)
    : QMainWindow(parent), bridge_(bridge) {
    setWindowTitle(QStringLiteral("MooSwitcher"));

    auto* central = new QWidget;
    auto* rootRow = new QHBoxLayout(central);
    auto* leftCol = new QVBoxLayout;
    rootRow->addLayout(leftCol, 1);

    multiview_ = new MultiviewWidget;
    leftCol->addWidget(multiview_, 1);
    connect(&bridge_, &EngineBridge::multiviewFrame, multiview_,
            &MultiviewWidget::setFrame);

    auto makeBusRow = [&](const QString& label, std::vector<QPushButton*>& store,
                          auto slot) {
        auto* row = new QHBoxLayout;
        auto* tag = new QLabel(label);
        tag->setMinimumWidth(44);
        tag->setStyleSheet(QStringLiteral("font-weight: bold;"));
        row->addWidget(tag);
        for (int i = 0; i < inputNames.size(); ++i) {
            auto* b = new QPushButton(
                QStringLiteral("%1\n%2").arg(i + 1).arg(inputNames[i]));
            b->setStyleSheet(kBtnBase);
            connect(b, &QPushButton::clicked, &bridge_,
                    [&bridge = bridge_, i, slot] { (bridge.*slot)(i); });
            store.push_back(b);
            row->addWidget(b);
        }
        row->addStretch(1);
        return row;
    };
    leftCol->addLayout(
        makeBusRow(QStringLiteral("PGM"), pgmBtns_, &EngineBridge::setProgram));
    leftCol->addLayout(
        makeBusRow(QStringLiteral("PVW"), pvwBtns_, &EngineBridge::setPreview));

    // -- transition row --
    auto* transRow = new QHBoxLayout;
    auto* cutBtn = new QPushButton(QStringLiteral("CUT"));
    cutBtn->setStyleSheet(kBtnBase);
    connect(cutBtn, &QPushButton::clicked, &bridge_, &EngineBridge::cut);
    transRow->addWidget(cutBtn);

    auto* autoBtn = new QPushButton(QStringLiteral("AUTO"));
    autoBtn->setStyleSheet(kBtnBase);
    connect(autoBtn, &QPushButton::clicked, &bridge_, &EngineBridge::autoTrans);
    transRow->addWidget(autoBtn);

    ftbBtn_ = new QPushButton(QStringLiteral("FTB"));
    ftbBtn_->setStyleSheet(kBtnBase);
    connect(ftbBtn_, &QPushButton::clicked, &bridge_, &EngineBridge::fadeToBlack);
    transRow->addWidget(ftbBtn_);

    transType_ = new QComboBox;
    transType_->addItems({QStringLiteral("Mix"), QStringLiteral("Wipe L>R"),
                          QStringLiteral("Wipe R>L"), QStringLiteral("Wipe T>B"),
                          QStringLiteral("Wipe B>T"), QStringLiteral("Box"),
                          QStringLiteral("Circle")});
    connect(transType_, &QComboBox::currentIndexChanged, this,
            &MainWindow::pushTransition);
    transRow->addWidget(transType_);

    transDur_ = new QSpinBox;
    transDur_->setRange(1, 600);
    transDur_->setValue(30);
    transDur_->setSuffix(QStringLiteral(" fr"));
    connect(transDur_, &QSpinBox::valueChanged, this, &MainWindow::pushTransition);
    transRow->addWidget(transDur_);
    transRow->addStretch(1);
    leftCol->addLayout(transRow);

    status_ = new QLabel;
    status_->setStyleSheet(QStringLiteral("font-family: monospace;"));
    leftCol->addWidget(status_);
    connect(&bridge_, &EngineBridge::statusText, status_, &QLabel::setText);

    // -- T-bar column --
    auto* tbarCol = new QVBoxLayout;
    auto* tbarTag = new QLabel(QStringLiteral("T-BAR"));
    tbarTag->setStyleSheet(QStringLiteral("font-weight: bold;"));
    tbarCol->addWidget(tbarTag, 0, Qt::AlignHCenter);
    tbar_ = new QSlider(Qt::Vertical);
    tbar_->setRange(0, 1000);
    tbar_->setMinimumHeight(240);
    connect(tbar_, &QSlider::sliderPressed, &bridge_, &EngineBridge::tbarBegin);
    connect(tbar_, &QSlider::sliderMoved, &bridge_,
            [&bridge = bridge_](int v) { bridge.tbarSet(float(v) / 1000.f); });
    connect(tbar_, &QSlider::sliderReleased, &bridge_, &EngineBridge::tbarEnd);
    tbarCol->addWidget(tbar_, 1, Qt::AlignHCenter);
    rootRow->addLayout(tbarCol);

    connect(&bridge_, &EngineBridge::stateChanged, this, &MainWindow::onState);

    new QShortcut(QKeySequence(Qt::Key_Space), this, [this] { bridge_.cut(); });
    new QShortcut(QKeySequence(Qt::Key_Return), this, [this] { bridge_.autoTrans(); });

    setCentralWidget(central);
    resize(1180, 800);
}

void MainWindow::pushTransition() {
    bridge_.setTransition(transType_->currentIndex(), transDur_->value(), 0.02f);
}

void MainWindow::onState(int program, int preview, bool inTransition, bool ftb) {
    if (program != lastProgram_ || preview != lastPreview_) {
        for (int i = 0; i < int(pgmBtns_.size()); ++i)
            pgmBtns_[size_t(i)]->setStyleSheet(i == program ? kBtnPgm : kBtnBase);
        for (int i = 0; i < int(pvwBtns_.size()); ++i)
            pvwBtns_[size_t(i)]->setStyleSheet(i == preview ? kBtnPvw : kBtnBase);
        lastProgram_ = program;
        lastPreview_ = preview;
    }
    if (ftb != lastFtb_) {
        ftbBtn_->setStyleSheet(ftb ? kBtnFtbOn : kBtnBase);
        lastFtb_ = ftb;
    }
    // Snap the T-bar home once a transition lands (unless the user holds it).
    if (!inTransition && !tbar_->isSliderDown() && tbar_->value() != 0) {
        tbar_->blockSignals(true);
        tbar_->setValue(0);
        tbar_->blockSignals(false);
    }
}

}  // namespace moo::ui
