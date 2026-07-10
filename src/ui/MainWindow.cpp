#include "ui/MainWindow.h"

#include <QHBoxLayout>
#include <QKeySequence>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>

#include "ui/MixerPanel.h"

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
                       ShowFile* showFile, const ShowFile::State* initial,
                       QWidget* parent)
    : QMainWindow(parent), bridge_(bridge), showFile_(showFile) {
    setWindowTitle(QStringLiteral("MooSwitcher"));
    if (initial) baseState_ = *initial;

    auto* central = new QWidget;
    auto* rootRow = new QHBoxLayout(central);
    auto* leftCol = new QVBoxLayout;
    rootRow->addLayout(leftCol, 1);

    multiview_ = new MultiviewWidget;
    leftCol->addWidget(multiview_, 1);
    connect(&bridge_, &EngineBridge::multiviewFrame, multiview_,
            &MultiviewWidget::setFrame);

    banner_ = new QLabel;
    banner_->setStyleSheet(QStringLiteral(
        "background: #7a1010; color: white; font-weight: bold; padding: 4px;"));
    banner_->hide();
    leftCol->addWidget(banner_);
    connect(&bridge_, &EngineBridge::healthChanged, this,
            [this](const QStringList& problems) {
                banner_->setText(problems.join(QStringLiteral("   |   ")));
                banner_->setVisible(!problems.isEmpty());
            });

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
    transDur_->setValue(initial ? initial->transDurTicks : 30);
    transDur_->setSuffix(QStringLiteral(" fr"));
    connect(transDur_, &QSpinBox::valueChanged, this, &MainWindow::pushTransition);
    transRow->addWidget(transDur_);
    transRow->addStretch(1);
    leftCol->addLayout(transRow);
    if (initial && initial->transType > 0 &&
        initial->transType < transType_->count())
        transType_->setCurrentIndex(initial->transType);  // fires pushTransition
    else
        pushTransition();  // push duration either way

    // -- DSK row: per keyer, on-air toggle + key source + fade duration --
    auto* dskRow = new QHBoxLayout;
    for (int k = 0; k < kDskCount; ++k) {
        dskBtns_[k] = new QPushButton(QStringLiteral("DSK %1").arg(k + 1));
        dskBtns_[k]->setStyleSheet(kBtnBase);
        connect(dskBtns_[k], &QPushButton::clicked, &bridge_,
                [&bridge = bridge_, k] { bridge.dskToggle(k); });
        dskRow->addWidget(dskBtns_[k]);

        dskSrc_[k] = new QComboBox;
        for (int i = 0; i < int(inputNames.size()); ++i)
            dskSrc_[k]->addItem(
                QStringLiteral("%1 %2").arg(i + 1).arg(inputNames[i].left(14)));
        connect(dskSrc_[k], &QComboBox::currentIndexChanged, &bridge_,
                [&bridge = bridge_, k](int src) { bridge.setDskSource(k, src); });
        dskRow->addWidget(dskSrc_[k]);

        dskFade_[k] = new QSpinBox;
        dskFade_[k]->setRange(1, 600);
        dskFade_[k]->setValue(30);
        dskFade_[k]->setSuffix(QStringLiteral(" fr"));
        connect(dskFade_[k], &QSpinBox::valueChanged, &bridge_,
                [&bridge = bridge_, k](int t) { bridge.setDskFade(k, t); });
        dskRow->addWidget(dskFade_[k]);
        if (k == 0) dskRow->addSpacing(16);
    }
    dskRow->addStretch(1);
    leftCol->addLayout(dskRow);

    if (bridge_.audioAvailable()) {
        auto* mixer = new MixerPanel(bridge_, inputNames);
        leftCol->addWidget(mixer);
        connect(&bridge_, &EngineBridge::audioLevels, mixer,
                &MixerPanel::onLevels);
        connect(&bridge_, &EngineBridge::inputNamesChanged, mixer,
                &MixerPanel::onInputNames);
    }
    connect(&bridge_, &EngineBridge::inputNamesChanged, this,
            &MainWindow::onInputNames);

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
    new QShortcut(QKeySequence(Qt::Key_F), this, [this] { bridge_.fadeToBlack(); });
    new QShortcut(QKeySequence(Qt::Key_D), this, [this] { bridge_.dskToggle(0); });
    new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_D), this,
                  [this] { bridge_.dskToggle(1); });
    for (int i = 0; i < int(inputNames.size()) && i < 9; ++i) {
        new QShortcut(QKeySequence(Qt::Key_1 + i), this,
                      [this, i] { bridge_.setProgram(i); });
        new QShortcut(QKeySequence(Qt::SHIFT | (Qt::Key_1 + i)), this,
                      [this, i] { bridge_.setPreview(i); });
    }

    if (initial) {
        bridge_.setProgram(initial->program);
        bridge_.setPreview(initial->preview);
        for (int k = 0; k < kDskCount; ++k) {
            const auto& d = initial->dsk[k];
            if (d.source > 0 && d.source < dskSrc_[k]->count())
                dskSrc_[k]->setCurrentIndex(d.source);  // fires setDskSource
            dskFade_[k]->setValue(d.fadeDurTicks);      // fires setDskFade
            // A restored on=true keyer fades in on startup (documented).
            if (d.on) bridge_.dskToggle(k);
        }
    }

    if (showFile_) {
        lastSaved_ = baseState_;
        auto* saver = new QTimer(this);
        saver->setInterval(2000);
        connect(saver, &QTimer::timeout, this, &MainWindow::saveShow);
        saver->start();
    }

    setCentralWidget(central);
    resize(1180, 800);
}

ShowFile::State MainWindow::collectState() const {
    ShowFile::State st = baseState_;
    st.cfg.inputs.clear();
    for (int i = 0; i < bridge_.inputCount(); ++i) {
        // The engine knows each input's true type; re-deriving it from the
        // ref would misfile OMT discovery names (no scheme) as NDI.
        st.cfg.inputs.push_back({InputSpec::Type(bridge_.inputType(i)),
                                 bridge_.inputRef(i).toStdString(),
                                 bridge_.inputSyncFrames(i)});
    }
    st.program = lastProgram_ < 0 ? 0 : lastProgram_;
    st.preview = lastPreview_ < 0 ? 1 : lastPreview_;
    st.transType = transType_->currentIndex();
    st.transDurTicks = transDur_->value();
    for (int k = 0; k < kDskCount; ++k)
        st.dsk[k] = {dskSrc_[k]->currentIndex(), dskFade_[k]->value(),
                     lastDskOn_[k]};
    st.cfg.masterAudioDelayMs = bridge_.masterDelayMs();
    st.chans.clear();
    for (int i = 0; i < bridge_.inputCount(); ++i)
        st.chans.push_back({bridge_.audioGain(i), bridge_.audioMute(i),
                            bridge_.audioSolo(i), bridge_.audioInputDelayMs(i)});
    return st;
}

void MainWindow::saveShow() {
    if (!showFile_) return;
    ShowFile::State st = collectState();
    if (everSaved_ && st == lastSaved_) return;
    showFile_->save(st);
    lastSaved_ = std::move(st);
    everSaved_ = true;
}

void MainWindow::pushTransition() {
    bridge_.setTransition(transType_->currentIndex(), transDur_->value(), 0.02f);
}

void MainWindow::onInputNames(const QStringList& refs) {
    for (int i = 0; i < refs.size(); ++i) {
        QString n = refs[i];
        if (n.startsWith(QStringLiteral("srt://")))
            n = QStringLiteral("SRT ") + n.mid(6);
        else if (n.startsWith(QStringLiteral("omt://")))
            n = QStringLiteral("OMT ") + n.mid(6);
        const QString text = QStringLiteral("%1\n%2").arg(i + 1).arg(n.left(14));
        if (i < int(pgmBtns_.size())) pgmBtns_[size_t(i)]->setText(text);
        if (i < int(pvwBtns_.size())) pvwBtns_[size_t(i)]->setText(text);
        for (int k = 0; k < kDskCount; ++k)
            if (i < dskSrc_[k]->count())
                dskSrc_[k]->setItemText(
                    i, QStringLiteral("%1 %2").arg(i + 1).arg(n.left(14)));
    }
}

void MainWindow::onState(int program, int preview, bool inTransition, bool ftb,
                         bool dsk1, bool dsk2) {
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
    const bool dskOn[kDskCount] = {dsk1, dsk2};
    for (int k = 0; k < kDskCount; ++k) {
        if (dskOn[k] == lastDskOn_[k]) continue;
        dskBtns_[k]->setStyleSheet(dskOn[k] ? kBtnFtbOn : kBtnBase);
        lastDskOn_[k] = dskOn[k];
    }
    // Snap the T-bar home once a transition lands (unless the user holds it).
    if (!inTransition && !tbar_->isSliderDown() && tbar_->value() != 0) {
        tbar_->blockSignals(true);
        tbar_->setValue(0);
        tbar_->blockSignals(false);
    }
}

}  // namespace moo::ui
