#include "ui/MainWindow.h"

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeySequence>
#include <QScrollArea>
#include <QShortcut>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "media/StillImage.h"
#include "ui/MixerPanel.h"

namespace moo::ui {

namespace {

constexpr auto kProductionStyle = R"QSS(
QMainWindow, QWidget#rootSurface {
    background: #080b10;
    color: #dbe3eb;
    font-family: "Inter", "Noto Sans", "DejaVu Sans", sans-serif;
    font-size: 12px;
}
QToolTip {
    background: #202a35;
    color: #f5f8fb;
    border: 1px solid #516174;
    border-radius: 5px;
    padding: 7px 9px;
}
QFrame#topBar {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                stop:0 #1b2530, stop:0.55 #131b24,
                                stop:1 #0d131a);
    border-bottom: 1px solid #314151;
}
QLabel#brandMark {
    color: #f7f9fb;
    font-size: 18px;
    font-weight: 800;
    letter-spacing: 2.4px;
}
QLabel#brandIcon { background: transparent; }
QLabel#brandCaption {
    color: #5eb4dc;
    font-size: 9px;
    font-weight: 800;
    letter-spacing: 1.5px;
}
QLabel#eyebrow, QLabel#subtleText {
    color: #7d8c9c;
    font-size: 10px;
    font-weight: 700;
    letter-spacing: 1px;
}
QLabel#clock {
    color: #e2e8ee;
    font-family: "JetBrains Mono", "DejaVu Sans Mono", monospace;
    font-size: 15px;
    font-weight: 700;
    padding-left: 3px;
}
QLabel#healthBadge, QLabel#busReadout, QLabel#formatState {
    background: #0c1219;
    border: 1px solid #344352;
    border-radius: 6px;
    color: #b9c5d0;
    font-size: 10px;
    font-weight: 800;
    padding: 7px 10px;
}
QLabel#healthBadge[state="good"] {
    color: #67e3aa;
    background: #0c1b17;
    border-color: #286248;
}
QLabel#healthBadge[state="bad"] {
    color: #ff7981;
    background: #251216;
    border-color: #77313a;
}
QLabel#busReadout {
    min-width: 118px;
    padding-left: 11px;
    padding-right: 11px;
}
QLabel#busReadout[bus="program"] {
    color: #ff8188;
    background: #211116;
    border-color: #7d303b;
    border-left: 3px solid #f04450;
}
QLabel#busReadout[bus="preview"] {
    color: #75e2a2;
    background: #0d1d17;
    border-color: #296a4a;
    border-left: 3px solid #34c37a;
}
QLabel#formatState[state="active"] {
    color: #67e3aa;
    background: #0d1d17;
    border-color: #286248;
}
QLabel#formatState[state="pending"] {
    color: #ffd071;
    background: #211a0c;
    border-color: #75571e;
}
QFrame#recordDeck, QFrame#outputDeck {
    background: #0b1117;
    border: 1px solid #2e3b48;
    border-radius: 7px;
}
QFrame#recordDeck[feed="program"] { border-color: #5d2931; }
QFrame#recordDeck[feed="clean"] { border-color: #28566a; }
QLabel#recordState {
    background: transparent;
    border: none;
    color: #71808e;
    font-family: "JetBrains Mono", "DejaVu Sans Mono", monospace;
    font-size: 9px;
    font-weight: 800;
    min-width: 30px;
    padding: 0 2px;
}
QLabel#recordState[state="recording"] { color: #ff8188; }
QLabel#recordState[state="pending"] { color: #ffd071; }
QLabel#recordState[state="error"] { color: #ffd071; }
QLabel#alertBanner {
    background: #3d171d;
    color: #ffc0c4;
    border-bottom: 1px solid #913641;
    font-weight: 700;
    padding: 8px 17px;
}
QFrame#monitorPanel, QFrame#modulePanel, QFrame#channelStrip {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                stop:0 #121a22, stop:1 #0d1319);
    border: 1px solid #2b3946;
    border-radius: 8px;
}
QFrame#monitorPanel { border-color: #30404e; }
QFrame#modulePanel[accent="red"]   { border-top: 2px solid #d23b48; }
QFrame#modulePanel[accent="green"] { border-top: 2px solid #35a86f; }
QFrame#modulePanel[accent="blue"]  { border-top: 2px solid #3e9bc7; }
QFrame#channelStrip {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                stop:0 #18222c, stop:1 #0c1117);
}
QFrame#channelStrip[master="true"] { border-top: 2px solid #4badd8; }
QLabel#sectionTitle {
    color: #e4eaf0;
    font-size: 11px;
    font-weight: 800;
    letter-spacing: 1.2px;
}
QLabel#sectionHint {
    color: #718092;
    font-size: 10px;
}
QLabel#busTag {
    background: #111922;
    border: 1px solid #344352;
    border-radius: 6px;
    color: #97a5b3;
    font-size: 10px;
    font-weight: 800;
    letter-spacing: 1px;
    padding: 5px;
}
QLabel#busTag[bus="program"] {
    color: #ff7b83;
    background: #211116;
    border-color: #6e2d36;
    border-left: 3px solid #f04450;
}
QLabel#busTag[bus="preview"] {
    color: #72dfa0;
    background: #0d1d17;
    border-color: #286044;
    border-left: 3px solid #34c37a;
}
QPushButton {
    outline: none;
}
QPushButton#sourceButton {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                stop:0 #293642, stop:0.12 #25313c,
                                stop:1 #151c24);
    border: 1px solid #405161;
    border-bottom: 2px solid #070a0e;
    border-radius: 6px;
    color: #d3dbe3;
    font-size: 11px;
    font-weight: 750;
    min-width: 88px;
    min-height: 47px;
    padding: 3px 7px;
}
QPushButton#sourceButton:hover {
    background: #2f3e4c;
    border-color: #6d8194;
    color: white;
}
QPushButton#sourceButton:pressed {
    background: #0f151b;
    border-bottom-width: 1px;
    padding-top: 5px;
}
QPushButton#sourceButton[tally="program"] {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                stop:0 #b8333e, stop:1 #701c26);
    border: 2px solid #ff5963;
    color: #ffffff;
}
QPushButton#sourceButton[tally="preview"] {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                stop:0 #27875a, stop:1 #164b34);
    border: 2px solid #56df91;
    color: #ffffff;
}
QPushButton#actionButton, QPushButton#keyButton {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                stop:0 #2c3945, stop:1 #172029);
    border: 1px solid #465a6c;
    border-bottom: 2px solid #070a0e;
    border-radius: 6px;
    color: #e5eaf0;
    font-size: 11px;
    font-weight: 850;
    min-height: 38px;
    padding: 4px 11px;
}
QPushButton#actionButton:hover, QPushButton#keyButton:hover {
    background: #354655; border-color: #71869a;
}
QPushButton#actionButton:pressed, QPushButton#keyButton:pressed {
    background: #11161b; border-bottom-width: 1px; padding-top: 6px;
}
QPushButton#actionButton:disabled, QPushButton#keyButton:disabled,
QPushButton#sourceButton:disabled {
    background: #11171d;
    border-color: #28333e;
    color: #53606c;
}
QPushButton#actionButton[action="cut"] { color: #ffd56c; }
QPushButton#actionButton[action="auto"] { color: #82d2fa; }
QPushButton#actionButton[action="ftb"] { color: #ff8188; }
QPushButton#recordButton {
    background: #171f28;
    border: 1px solid #70303a;
    border-radius: 5px;
    color: #ff8d93;
    font-size: 10px;
    font-weight: 850;
    min-height: 27px;
    padding: 3px 9px;
}
QPushButton#recordButton[feed="clean"] {
    color: #77ccec;
    border-color: #2c6b85;
}
QPushButton#recordButton:hover {
    background: #252f3a;
    border-color: #b04a56;
}
QPushButton#recordButton[feed="clean"]:hover {
    border-color: #4fa6c9;
}
QPushButton#recordButton:checked {
    background: #8a222c;
    border-color: #ff5963;
    color: white;
}
QPushButton#recordButton[feed="clean"]:checked {
    background: #1e607b;
    border-color: #70d3f6;
    color: white;
}
QPushButton#actionButton[active="true"] {
    background: #254c65; border-color: #62b7e5; color: white;
}
QPushButton#actionButton[action="ftb"][active="true"],
QPushButton#keyButton[active="true"] {
    background: #7c2028; border-color: #ff5963; color: white;
}
QPushButton#channelName {
    background: #0d1217;
    border: 1px solid #2f3944;
    border-radius: 4px;
    color: #cbd3db;
    font-size: 10px;
    font-weight: 750;
    min-height: 30px;
    padding: 3px 6px;
}
QPushButton#channelName:hover { border-color: #4d91b4; color: white; }
QPushButton#mixerToggle {
    background: #151b21;
    border: 1px solid #35404b;
    border-radius: 4px;
    color: #7d8996;
    font-size: 9px;
    font-weight: 850;
    min-height: 27px;
    padding: 2px 5px;
}
QPushButton#mixerToggle:hover { color: #dce2e8; border-color: #5a6877; }
QPushButton#mixerToggle[kind="mute"]:checked {
    background: #7a242b; border-color: #ef5962; color: white;
}
QPushButton#mixerToggle[kind="solo"]:checked {
    background: #8a6818; border-color: #e5bd4d; color: #fff7d8;
}
QLabel#channelIndex, QLabel#gainReadout, QLabel#trimReadout {
    color: #697686;
    font-size: 9px;
    font-weight: 750;
}
QLabel#gainReadout { color: #91a1b1; font-family: "DejaVu Sans Mono", monospace; }
QLabel#meterScale { color: #596675; font-size: 8px; }
QFrame#keyerCard {
    background: #0b1117;
    border: 1px solid #2c3a47;
    border-radius: 7px;
}
QLabel#keyerTitle {
    color: #d4dce4;
    font-size: 10px;
    font-weight: 800;
    letter-spacing: 0.7px;
}
QComboBox, QSpinBox, QLineEdit {
    background: #090e13;
    border: 1px solid #3a4a59;
    border-radius: 5px;
    color: #d9e0e7;
    min-height: 26px;
    padding: 2px 7px;
    selection-background-color: #326b8c;
}
QComboBox:hover, QSpinBox:hover, QLineEdit:hover { border-color: #617487; }
QComboBox:focus, QSpinBox:focus, QLineEdit:focus { border-color: #55abd2; }
QComboBox#outputResolution, QComboBox#outputFrameRate {
  min-width: 108px; font-size: 10px; font-weight: 750;
  background: #0d141b;
  border-color: #334757;
}
QComboBox::drop-down { border: none; width: 20px; }
QComboBox QAbstractItemView {
    background: #151b22;
    color: #d6dce2;
    border: 1px solid #465361;
    selection-background-color: #315f79;
    padding: 3px;
}
QSpinBox::up-button, QSpinBox::down-button { width: 14px; background: #1b232c; border: none; }
QSlider::groove:vertical {
    background: #07090c;
    border: 1px solid #303944;
    border-radius: 4px;
    width: 8px;
}
QSlider::handle:vertical {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                                stop:0 #667382, stop:0.5 #d7dde3, stop:1 #667382);
    border: 1px solid #e9edf1;
    border-radius: 4px;
    height: 20px;
    margin: 0 -10px;
}
QSlider::sub-page:vertical, QSlider::add-page:vertical {
    background: #0a0e12;
    border-radius: 3px;
}
QSlider#tBar::groove:vertical {
    background: qlineargradient(x1:0, y1:1, x2:0, y2:0,
                                stop:0 #101820, stop:0.5 #1c3544,
                                stop:1 #24546a);
    border: 1px solid #40596a;
    border-radius: 5px;
    width: 10px;
}
QSlider#tBar::handle:vertical {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                                stop:0 #7891a3, stop:0.5 #f3f7fa,
                                stop:1 #7891a3);
    border: 1px solid #f5f8fa;
    border-radius: 5px;
    height: 22px;
    margin: 0 -11px;
}
QTabWidget#workspaceTabs::pane {
    background: #0c1218;
    border: 1px solid #2c3a47;
    border-radius: 7px;
    top: -1px;
}
QTabWidget#workspaceTabs QTabBar::tab {
    background: #0e151c;
    border: 1px solid #2c3a47;
    border-bottom: none;
    color: #778798;
    font-size: 10px;
    font-weight: 850;
    letter-spacing: 1px;
    min-width: 132px;
    padding: 9px 18px;
    margin-right: 2px;
}
QTabWidget#workspaceTabs QTabBar::tab:selected {
    background: #18232d;
    color: #edf2f6;
    border-top: 2px solid #53add5;
}
QTabWidget#workspaceTabs QTabBar::tab:hover:!selected { color: #bac3cc; }
QScrollArea { background: transparent; border: none; }
QScrollArea > QWidget > QWidget { background: transparent; }
QScrollBar:horizontal, QScrollBar:vertical {
    background: #0b0f13;
    border: none;
    height: 9px;
    width: 9px;
}
QScrollBar::handle:horizontal, QScrollBar::handle:vertical {
    background: #38434e;
    border-radius: 4px;
    min-width: 30px;
    min-height: 30px;
}
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QSplitter::handle {
    background: #080b10;
    border-top: 1px solid #18222c;
    border-bottom: 1px solid #05070a;
    height: 6px;
}
QLabel#statusLine {
    background: #070a0e;
    border-top: 1px solid #1d2832;
    color: #657789;
    font-family: "JetBrains Mono", "DejaVu Sans Mono", monospace;
    font-size: 9px;
    padding: 4px 8px;
}
QDialog, QListWidget {
    background: #10171e;
    color: #dbe3eb;
}
QListWidget { border: 1px solid #35414d; border-radius: 4px; }
QListWidget::item { padding: 7px; }
QListWidget::item:selected { background: #315f79; }
QDialogButtonBox QPushButton {
    background: #1b2732;
    border: 1px solid #465a6c;
    border-radius: 5px;
    color: #e4eaf0;
    font-weight: 750;
    min-width: 76px;
    min-height: 28px;
    padding: 3px 10px;
}
QDialogButtonBox QPushButton:hover {
    background: #2c3c4a;
    border-color: #6e8498;
}
)QSS";

QString displayName(QString ref) {
    ref = ref.trimmed();
    if (ref.startsWith(QStringLiteral("srt://"), Qt::CaseInsensitive))
        ref = QStringLiteral("SRT · ") + ref.mid(6);
    else if (ref.startsWith(QStringLiteral("omt://"), Qt::CaseInsensitive))
        ref = QStringLiteral("OMT · ") + ref.mid(6);
    else if (QFileInfo(ref).isAbsolute())
        ref = (media::isStillImagePath(ref.toStdString())
                   ? QStringLiteral("STILL · ")
                   : QStringLiteral("MEDIA · ")) +
              QFileInfo(ref).fileName();
    if (ref.isEmpty()) return QStringLiteral("NO SOURCE");
    return ref;
}

void setVisualState(QWidget* widget, const char* property, const QVariant& value) {
    if (widget->property(property) == value) return;
    widget->setProperty(property, value);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

QLabel* makeSectionTitle(const QString& title, const QString& hint = {}) {
    auto* label = new QLabel(hint.isEmpty()
                                 ? title
                                 : QStringLiteral("%1   ·   %2").arg(title, hint));
    label->setObjectName(QStringLiteral("sectionTitle"));
    return label;
}

QFrame* makeModule(const char* accent = nullptr) {
    auto* panel = new QFrame;
    panel->setObjectName(QStringLiteral("modulePanel"));
    if (accent) panel->setProperty("accent", accent);
    return panel;
}

struct ResolutionPreset {
    int width;
    int height;
    const char* label;
};

constexpr ResolutionPreset kResolutionPresets[] = {
    {1280, 720, "1280 × 720  HD"},
    {1920, 1080, "1920 × 1080  FHD"},
    {2560, 1440, "2560 × 1440  QHD"},
    {3840, 2160, "3840 × 2160  UHD"},
    {4096, 2160, "4096 × 2160  DCI 4K"},
    {7680, 4320, "7680 × 4320  8K"},
};

struct FrameRatePreset {
    qlonglong numerator;
    qlonglong denominator;
    const char* label;
};

constexpr FrameRatePreset kFrameRatePresets[] = {
    {24000, 1001, "23.98p"}, {24, 1, "24p"},
    {25, 1, "25p"},         {30000, 1001, "29.97p"},
    {30, 1, "30p"},         {50, 1, "50p"},
    {60000, 1001, "59.94p"}, {60, 1, "60p"},
};

int findResolution(const QComboBox* combo, int width, int height) {
    for (int i = 0; i < combo->count(); ++i)
        if (combo->itemData(i).toInt() == width &&
            combo->itemData(i, Qt::UserRole + 1).toInt() == height)
            return i;
    return -1;
}

int findFrameRate(const QComboBox* combo, qlonglong numerator,
                  qlonglong denominator) {
    for (int i = 0; i < combo->count(); ++i)
        if (combo->itemData(i).toLongLong() == numerator &&
            combo->itemData(i, Qt::UserRole + 1).toLongLong() == denominator)
            return i;
    return -1;
}

QString decimalFrameRate(qlonglong numerator, qlonglong denominator) {
    QString rate = QString::number(double(numerator) / double(denominator), 'f', 3);
    while (rate.endsWith(QLatin1Char('0'))) rate.chop(1);
    if (rate.endsWith(QLatin1Char('.'))) rate.chop(1);
    return rate;
}

}  // namespace

MainWindow::MainWindow(EngineBridge& bridge, const QStringList& inputNames,
                       ShowFile* showFile, const ShowFile::State* initial,
                       QWidget* parent)
    : QMainWindow(parent), bridge_(bridge), showFile_(showFile),
      inputNames_(inputNames) {
    setWindowTitle(QStringLiteral("MooSwitcher · Live Production"));
    setWindowIcon(QIcon(QStringLiteral(":/branding/cow-switcher-logo.svg")));
    setStyleSheet(QLatin1String(kProductionStyle));
    setMinimumSize(1050, 720);
    activeOutput_ = bridge_.outputFormat();
    if (initial)
        baseState_ = *initial;
    else
        baseState_.cfg.show = activeOutput_;

    auto* central = new QWidget;
    central->setObjectName(QStringLiteral("rootSurface"));
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Persistent top strip: show identity, bus tallies, clock, and health.
    auto* topBar = new QFrame;
    topBar->setObjectName(QStringLiteral("topBar"));
    topBar->setFixedHeight(64);
    auto* topRow = new QHBoxLayout(topBar);
    topRow->setContentsMargins(16, 8, 14, 8);
    topRow->setSpacing(9);

    auto* brandIcon = new QLabel;
    brandIcon->setObjectName(QStringLiteral("brandIcon"));
    brandIcon->setFixedSize(44, 44);
    brandIcon->setPixmap(
        QIcon(QStringLiteral(":/branding/cow-switcher-logo.svg")).pixmap(44, 44));
    brandIcon->setToolTip(QStringLiteral("MooSwitcher"));
    topRow->addWidget(brandIcon);

    auto* brandCol = new QVBoxLayout;
    brandCol->setSpacing(0);
    auto* brand = new QLabel(QStringLiteral("MOO//SWITCHER"));
    brand->setObjectName(QStringLiteral("brandMark"));
    auto* brandCaption = new QLabel(QStringLiteral("LIVE PRODUCTION SYSTEM"));
    brandCaption->setObjectName(QStringLiteral("brandCaption"));
    brandCol->addWidget(brand);
    brandCol->addWidget(brandCaption);
    topRow->addLayout(brandCol);
    topRow->addStretch(2);

    programReadout_ = new QLabel;
    programReadout_->setObjectName(QStringLiteral("busReadout"));
    programReadout_->setProperty("bus", "program");
    programReadout_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    previewReadout_ = new QLabel;
    previewReadout_->setObjectName(QStringLiteral("busReadout"));
    previewReadout_->setProperty("bus", "preview");
    previewReadout_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    topRow->addWidget(programReadout_);
    topRow->addWidget(previewReadout_);

    auto* programRecordDeck = new QFrame;
    programRecordDeck->setObjectName(QStringLiteral("recordDeck"));
    programRecordDeck->setProperty("feed", "program");
    auto* programRecordRow = new QHBoxLayout(programRecordDeck);
    programRecordRow->setContentsMargins(4, 3, 5, 3);
    programRecordRow->setSpacing(4);
    recordBtn_ = new QPushButton(QStringLiteral("●  RECORD"));
    recordBtn_->setObjectName(QStringLiteral("recordButton"));
    recordBtn_->setProperty("feed", "program");
    recordBtn_->setCheckable(true);
    recordBtn_->setToolTip(
        QStringLiteral("Record the program mix as HEVC/AAC Matroska"));
    programRecordRow->addWidget(recordBtn_);
    recordState_ = new QLabel(QStringLiteral("IDLE"));
    recordState_->setObjectName(QStringLiteral("recordState"));
    recordState_->setProperty("state", "idle");
    recordState_->setProperty("feed", "program");
    programRecordRow->addWidget(recordState_);
    topRow->addWidget(programRecordDeck);
    connect(recordBtn_, &QPushButton::clicked, this, [this](bool checked) {
        if (!checked) {
            bridge_.stopRecording();
            recordState_->setText(QStringLiteral("STOPPING…"));
            setVisualState(recordState_, "state", "pending");
            return;
        }

        QString directory =
            QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
        if (directory.isEmpty()) directory = QDir::homePath();
        const QString suggested =
            QDir(directory).filePath(
                QStringLiteral("MooSwitcher-%1.mkv")
                    .arg(QDateTime::currentDateTime().toString(
                        QStringLiteral("yyyyMMdd-HHmmss"))));
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Record program"), suggested,
            QStringLiteral("Matroska video (*.mkv)"));
        if (path.isEmpty()) {
            recordBtn_->setChecked(false);
            return;
        }
        bridge_.startRecording(
            path.endsWith(QStringLiteral(".mkv"), Qt::CaseInsensitive)
                ? path
                : path + QStringLiteral(".mkv"));
        recordState_->setText(QStringLiteral("STARTING…"));
        setVisualState(recordState_, "state", "pending");
    });

    auto* cleanRecordDeck = new QFrame;
    cleanRecordDeck->setObjectName(QStringLiteral("recordDeck"));
    cleanRecordDeck->setProperty("feed", "clean");
    auto* cleanRecordRow = new QHBoxLayout(cleanRecordDeck);
    cleanRecordRow->setContentsMargins(4, 3, 5, 3);
    cleanRecordRow->setSpacing(4);
    cleanRecordBtn_ = new QPushButton(QStringLiteral("●  CLEAN"));
    cleanRecordBtn_->setObjectName(QStringLiteral("recordButton"));
    cleanRecordBtn_->setProperty("feed", "clean");
    cleanRecordBtn_->setCheckable(true);
    cleanRecordBtn_->setToolTip(QStringLiteral(
        "Record the switched program without downstream key graphics"));
    cleanRecordRow->addWidget(cleanRecordBtn_);
    cleanRecordState_ = new QLabel(QStringLiteral("IDLE"));
    cleanRecordState_->setObjectName(QStringLiteral("recordState"));
    cleanRecordState_->setProperty("state", "idle");
    cleanRecordState_->setProperty("feed", "clean");
    cleanRecordRow->addWidget(cleanRecordState_);
    topRow->addWidget(cleanRecordDeck);
    connect(cleanRecordBtn_, &QPushButton::clicked, this,
            [this](bool checked) {
        if (!checked) {
            bridge_.stopCleanRecording();
            cleanRecordState_->setText(QStringLiteral("STOPPING…"));
            setVisualState(cleanRecordState_, "state", "pending");
            return;
        }

        QString directory =
            QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
        if (directory.isEmpty()) directory = QDir::homePath();
        const QString suggested = QDir(directory).filePath(
            QStringLiteral("MooSwitcher-Clean-%1.mkv")
                .arg(QDateTime::currentDateTime().toString(
                    QStringLiteral("yyyyMMdd-HHmmss"))));
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Record clean feed"), suggested,
            QStringLiteral("Matroska video (*.mkv)"));
        if (path.isEmpty()) {
            cleanRecordBtn_->setChecked(false);
            return;
        }
        bridge_.startCleanRecording(
            path.endsWith(QStringLiteral(".mkv"), Qt::CaseInsensitive)
                ? path
                : path + QStringLiteral(".mkv"));
        cleanRecordState_->setText(QStringLiteral("STARTING…"));
        setVisualState(cleanRecordState_, "state", "pending");
    });

    healthBadge_ = new QLabel(QStringLiteral("●  ENGINE ONLINE"));
    healthBadge_->setObjectName(QStringLiteral("healthBadge"));
    healthBadge_->setProperty("state", "good");
    topRow->addWidget(healthBadge_);

    clock_ = new QLabel;
    clock_->setObjectName(QStringLiteral("clock"));
    clock_->setMinimumWidth(80);
    clock_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    topRow->addWidget(clock_);
    auto updateClock = [this] {
        clock_->setText(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")));
    };
    updateClock();
    auto* clockTimer = new QTimer(this);
    connect(clockTimer, &QTimer::timeout, this, updateClock);
    clockTimer->start(1000);
    root->addWidget(topBar);

    banner_ = new QLabel;
    banner_->setObjectName(QStringLiteral("alertBanner"));
    banner_->hide();
    root->addWidget(banner_);
    connect(&bridge_, &EngineBridge::healthChanged, this,
            [this](const QStringList& problems) {
                const bool good = problems.isEmpty();
                banner_->setText(QStringLiteral("SIGNAL WARNING   ·   ") +
                                 problems.join(QStringLiteral("   |   ")));
                banner_->setVisible(!good);
                healthBadge_->setText(good ? QStringLiteral("●  ENGINE ONLINE")
                                           : QStringLiteral("●  ATTENTION"));
                setVisualState(healthBadge_, "state", good ? "good" : "bad");
            });

    auto* workSplitter = new QSplitter(Qt::Vertical);
    workSplitter->setChildrenCollapsible(false);
    workSplitter->setHandleWidth(5);

    // The monitor is deliberately dominant; all control chrome stays outside
    // the image so the video surface remains uncluttered.
    auto* monitorPanel = new QFrame;
    monitorPanel->setObjectName(QStringLiteral("monitorPanel"));
    auto* monitorCol = new QVBoxLayout(monitorPanel);
    monitorCol->setContentsMargins(7, 7, 7, 7);
    monitorCol->setSpacing(7);
    auto* monitorHeader = new QHBoxLayout;
    monitorHeader->setContentsMargins(7, 1, 3, 1);
    monitorHeader->addWidget(
        makeSectionTitle(QStringLiteral("MULTIVIEW"),
                         QStringLiteral("L-CLICK PREVIEW  /  R-CLICK PROGRAM")));
    monitorHeader->addStretch(1);
    auto* outputDeck = new QFrame;
    outputDeck->setObjectName(QStringLiteral("outputDeck"));
    auto* outputRow = new QHBoxLayout(outputDeck);
    outputRow->setContentsMargins(7, 3, 4, 3);
    outputRow->setSpacing(5);
    auto* outputLabel = new QLabel(QStringLiteral("OUTPUT FORMAT"));
    outputLabel->setObjectName(QStringLiteral("eyebrow"));
    outputRow->addWidget(outputLabel);

    outputResolution_ = new QComboBox;
    outputResolution_->setObjectName(QStringLiteral("outputResolution"));
    outputResolution_->setToolTip(
        QStringLiteral("Program output resolution (restart required to apply)"));
    for (const auto& preset : kResolutionPresets) {
        outputResolution_->addItem(QString::fromUtf8(preset.label), preset.width);
        outputResolution_->setItemData(outputResolution_->count() - 1,
                                       preset.height, Qt::UserRole + 1);
    }
    int resolutionIndex = findResolution(outputResolution_, activeOutput_.width,
                                         activeOutput_.height);
    if (resolutionIndex < 0) {
        outputResolution_->addItem(
            QStringLiteral("%1 × %2  CUSTOM")
                .arg(activeOutput_.width)
                .arg(activeOutput_.height),
            activeOutput_.width);
        resolutionIndex = outputResolution_->count() - 1;
        outputResolution_->setItemData(resolutionIndex, activeOutput_.height,
                                       Qt::UserRole + 1);
    }
    outputResolution_->setCurrentIndex(resolutionIndex);
    outputRow->addWidget(outputResolution_);

    outputFrameRate_ = new QComboBox;
    outputFrameRate_->setObjectName(QStringLiteral("outputFrameRate"));
    outputFrameRate_->setToolTip(
        QStringLiteral("Progressive program output frame rate (restart required to apply)"));
    for (const auto& preset : kFrameRatePresets) {
        outputFrameRate_->addItem(QString::fromUtf8(preset.label), preset.numerator);
        outputFrameRate_->setItemData(outputFrameRate_->count() - 1,
                                      preset.denominator, Qt::UserRole + 1);
    }
    int rateIndex = findFrameRate(outputFrameRate_, activeOutput_.fpsN,
                                  activeOutput_.fpsD);
    if (rateIndex < 0) {
        outputFrameRate_->addItem(
            decimalFrameRate(activeOutput_.fpsN, activeOutput_.fpsD) +
                QStringLiteral("p  CUSTOM"),
            qlonglong(activeOutput_.fpsN));
        rateIndex = outputFrameRate_->count() - 1;
        outputFrameRate_->setItemData(rateIndex, qlonglong(activeOutput_.fpsD),
                                      Qt::UserRole + 1);
    }
    outputFrameRate_->setCurrentIndex(rateIndex);
    outputRow->addWidget(outputFrameRate_);

    outputFormatState_ = new QLabel;
    outputFormatState_->setObjectName(QStringLiteral("formatState"));
    outputRow->addWidget(outputFormatState_);
    monitorHeader->addWidget(outputDeck);
    auto outputChanged = [this](int) {
        refreshOutputFormatState();
        saveShow();
    };
    connect(outputResolution_, &QComboBox::currentIndexChanged, this,
            outputChanged);
    connect(outputFrameRate_, &QComboBox::currentIndexChanged, this,
            outputChanged);
    refreshOutputFormatState();
    monitorCol->addLayout(monitorHeader);

    multiview_ = new MultiviewWidget;
    multiview_->setInputCount(inputNames.size());
    monitorCol->addWidget(multiview_, 1);
    connect(&bridge_, &EngineBridge::multiviewFrame, multiview_,
            &MultiviewWidget::setFrame);
    connect(multiview_, &MultiviewWidget::previewSourceRequested, &bridge_,
            &EngineBridge::setPreview);
    connect(multiview_, &MultiviewWidget::programSourceRequested, &bridge_,
            &EngineBridge::setProgram);
    workSplitter->addWidget(monitorPanel);

    // -- Persistent M/E control surface ---------------------------------------
    // Only the left-hand workspace is tabbed. Transition and keyer controls
    // remain available while the source buses are replaced by the audio mixer.
    auto* controlSurface = new QWidget;
    controlSurface->setMinimumHeight(365);
    auto* switcherRow = new QHBoxLayout(controlSurface);
    // The source canvas may be wider than its viewport. Keep that size hint
    // local to the scroll area instead of letting it inflate the whole window
    // and squeeze the persistent header on smaller displays.
    switcherRow->setSizeConstraint(QLayout::SetNoConstraint);
    switcherRow->setContentsMargins(8, 8, 8, 8);
    switcherRow->setSpacing(8);

    auto* sourceTabs = new QTabWidget;
    sourceTabs->setObjectName(QStringLiteral("workspaceTabs"));
    sourceTabs->setDocumentMode(true);

    auto* buses = makeModule("red");
    auto* busesCol = new QVBoxLayout(buses);
    busesCol->setContentsMargins(11, 9, 11, 9);
    busesCol->setSpacing(8);
    auto* busesHeader = new QHBoxLayout;
    busesHeader->addWidget(
        makeSectionTitle(QStringLiteral("M/E 1"), QStringLiteral("SOURCE BUSES")));
    busesHeader->addStretch(1);
    auto* shortcutHint = new QLabel(QStringLiteral("SPACE  CUT     ENTER  AUTO"));
    shortcutHint->setObjectName(QStringLiteral("sectionHint"));
    busesHeader->addWidget(shortcutHint);
    busesCol->addLayout(busesHeader);

    auto* busCanvas = new QWidget;
    auto* busRows = new QVBoxLayout(busCanvas);
    busRows->setContentsMargins(0, 0, 0, 0);
    busRows->setSpacing(8);
    auto makeBusRow = [&](const QString& title, const QString& sub,
                          const char* bus, std::vector<QPushButton*>& store,
                          auto slot) {
        auto* row = new QHBoxLayout;
        row->setSpacing(7);
        auto* tag = new QLabel(QStringLiteral("%1\n%2").arg(title, sub));
        tag->setObjectName(QStringLiteral("busTag"));
        tag->setProperty("bus", bus);
        tag->setAlignment(Qt::AlignCenter);
        tag->setFixedSize(92, 55);
        row->addWidget(tag);
        for (int i = 0; i < inputNames.size(); ++i) {
            auto* button = new QPushButton;
            button->setObjectName(QStringLiteral("sourceButton"));
            button->setProperty("tally", "idle");
            button->setToolTip(
                QStringLiteral("Send input %1 to %2").arg(i + 1).arg(title));
            connect(button, &QPushButton::clicked, &bridge_,
                    [&bridge = bridge_, i, slot] { (bridge.*slot)(i); });
            store.push_back(button);
            row->addWidget(button, 1);
        }
        busRows->addLayout(row);
    };
    makeBusRow(QStringLiteral("PROGRAM"), QStringLiteral("ON AIR"), "program",
               pgmBtns_, &EngineBridge::setProgram);
    makeBusRow(QStringLiteral("PREVIEW"), QStringLiteral("NEXT"), "preview",
               pvwBtns_, &EngineBridge::setPreview);
    busRows->addStretch(1);

    auto* busScroll = new QScrollArea;
    busScroll->setWidgetResizable(true);
    busScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    busScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    busScroll->setWidget(busCanvas);
    busesCol->addWidget(busScroll, 1);
    sourceTabs->addTab(buses, QStringLiteral("SWITCHER"));

    if (bridge_.audioAvailable()) {
        auto* mixer = new MixerPanel(bridge_, inputNames);
        sourceTabs->addTab(mixer, QStringLiteral("AUDIO MIXER"));
        connect(&bridge_, &EngineBridge::audioLevels, mixer,
                &MixerPanel::onLevels);
        connect(&bridge_, &EngineBridge::inputNamesChanged, mixer,
                &MixerPanel::onInputNames);
    }

    auto* mediaPanel = new QWidget;
    auto* mediaRoot = new QVBoxLayout(mediaPanel);
    mediaRoot->setContentsMargins(11, 9, 11, 10);
    mediaRoot->setSpacing(8);
    mediaRoot->addWidget(makeSectionTitle(
        QStringLiteral("MEDIA PLAYERS"),
        QStringLiteral("LOCAL H.264 / HEVC PLAYLISTS PATCHED AS INPUTS")));
    for (int i = 0; i < bridge_.inputCount(); ++i) {
        auto* card = new QFrame;
        card->setObjectName(QStringLiteral("modulePanel"));
        auto* row = new QHBoxLayout(card);
        row->setContentsMargins(10, 7, 10, 7);
        row->setSpacing(8);

        MediaRow controls;
        controls.name = new QLabel;
        controls.name->setObjectName(QStringLiteral("sectionTitle"));
        controls.name->setMinimumWidth(220);
        row->addWidget(controls.name, 1);
        controls.time = new QLabel(QStringLiteral("— / —"));
        controls.time->setObjectName(QStringLiteral("subtleText"));
        controls.time->setMinimumWidth(260);
        controls.time->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(controls.time);
        controls.previous = new QPushButton(QStringLiteral("◀  PREV"));
        controls.previous->setObjectName(QStringLiteral("actionButton"));
        row->addWidget(controls.previous);
        controls.restart = new QPushButton(QStringLiteral("↺  RESTART"));
        controls.restart->setObjectName(QStringLiteral("actionButton"));
        row->addWidget(controls.restart);
        controls.play = new QPushButton(QStringLiteral("PAUSE"));
        controls.play->setObjectName(QStringLiteral("actionButton"));
        controls.play->setCheckable(true);
        row->addWidget(controls.play);
        controls.next = new QPushButton(QStringLiteral("NEXT  ▶"));
        controls.next->setObjectName(QStringLiteral("actionButton"));
        row->addWidget(controls.next);
        controls.loop = new QPushButton(QStringLiteral("LOOP LIST"));
        controls.loop->setObjectName(QStringLiteral("actionButton"));
        controls.loop->setCheckable(true);
        row->addWidget(controls.loop);

        connect(controls.play, &QPushButton::clicked, &bridge_,
                [&bridge = bridge_, i](bool playing) {
                    bridge.setMediaPlaying(i, playing);
                });
        connect(controls.restart, &QPushButton::clicked, &bridge_,
                [&bridge = bridge_, i] { bridge.restartMedia(i); });
        connect(controls.previous, &QPushButton::clicked, &bridge_,
                [&bridge = bridge_, i] { bridge.stepMedia(i, -1); });
        connect(controls.next, &QPushButton::clicked, &bridge_,
                [&bridge = bridge_, i] { bridge.stepMedia(i, 1); });
        connect(controls.loop, &QPushButton::clicked, &bridge_,
                [&bridge = bridge_, i](bool loop) {
                    bridge.setMediaLoop(i, loop);
                });
        mediaRows_.push_back(controls);
        mediaRoot->addWidget(card);
    }
    mediaRoot->addStretch(1);
    sourceTabs->addTab(mediaPanel, QStringLiteral("MEDIA"));

    switcherRow->addWidget(sourceTabs, 1);

    // Transition controls and T-bar share a module, mirroring a hardware M/E.
    auto* transition = makeModule("blue");
    transition->setMinimumWidth(315);
    transition->setMaximumWidth(355);
    auto* transitionRow = new QHBoxLayout(transition);
    transitionRow->setContentsMargins(11, 9, 11, 9);
    transitionRow->setSpacing(12);
    auto* transitionControls = new QVBoxLayout;
    transitionControls->setSpacing(7);
    transitionControls->addWidget(
        makeSectionTitle(QStringLiteral("TRANSITION"), QStringLiteral("M/E 1")));

    auto* styleRow = new QHBoxLayout;
    transType_ = new QComboBox;
    transType_->addItems({QStringLiteral("Mix"), QStringLiteral("Wipe L → R"),
                          QStringLiteral("Wipe R → L"), QStringLiteral("Wipe T → B"),
                          QStringLiteral("Wipe B → T"), QStringLiteral("Box"),
                          QStringLiteral("Circle")});
    transType_->setToolTip(QStringLiteral("Transition style"));
    styleRow->addWidget(transType_, 1);
    transDur_ = new QSpinBox;
    transDur_->setRange(1, 600);
    transDur_->setValue(initial ? initial->transDurTicks : 30);
    transDur_->setSuffix(QStringLiteral(" fr"));
    transDur_->setToolTip(QStringLiteral("Transition duration in output frames"));
    styleRow->addWidget(transDur_);
    transitionControls->addLayout(styleRow);

    auto* actionRow = new QHBoxLayout;
    cutBtn_ = new QPushButton(QStringLiteral("CUT"));
    cutBtn_->setObjectName(QStringLiteral("actionButton"));
    cutBtn_->setProperty("action", "cut");
    connect(cutBtn_, &QPushButton::clicked, &bridge_, &EngineBridge::cut);
    actionRow->addWidget(cutBtn_, 1);
    autoBtn_ = new QPushButton(QStringLiteral("AUTO"));
    autoBtn_->setObjectName(QStringLiteral("actionButton"));
    autoBtn_->setProperty("action", "auto");
    autoBtn_->setProperty("active", false);
    connect(autoBtn_, &QPushButton::clicked, &bridge_, &EngineBridge::autoTrans);
    actionRow->addWidget(autoBtn_, 1);
    transitionControls->addLayout(actionRow);

    ftbBtn_ = new QPushButton(QStringLiteral("FADE TO BLACK"));
    ftbBtn_->setObjectName(QStringLiteral("actionButton"));
    ftbBtn_->setProperty("action", "ftb");
    ftbBtn_->setProperty("active", false);
    connect(ftbBtn_, &QPushButton::clicked, &bridge_, &EngineBridge::fadeToBlack);
    transitionControls->addWidget(ftbBtn_);
    transitionControls->addStretch(1);
    transitionRow->addLayout(transitionControls, 1);

    auto* tbarCol = new QVBoxLayout;
    tbarCol->setSpacing(3);
    auto* tbarTag = new QLabel(QStringLiteral("T-BAR"));
    tbarTag->setObjectName(QStringLiteral("eyebrow"));
    tbarTag->setAlignment(Qt::AlignCenter);
    tbarCol->addWidget(tbarTag);
    tbar_ = new QSlider(Qt::Vertical);
    tbar_->setObjectName(QStringLiteral("tBar"));
    tbar_->setRange(0, 1000);
    tbar_->setMinimumHeight(92);
    tbar_->setToolTip(QStringLiteral("Drag for a manual transition"));
    connect(tbar_, &QSlider::sliderPressed, &bridge_, &EngineBridge::tbarBegin);
    connect(tbar_, &QSlider::sliderMoved, &bridge_,
            [&bridge = bridge_](int value) {
                bridge.tbarSet(float(value) / 1000.f);
            });
    connect(tbar_, &QSlider::sliderReleased, &bridge_, &EngineBridge::tbarEnd);
    tbarCol->addWidget(tbar_, 1, Qt::AlignHCenter);
    transitionRow->addLayout(tbarCol);
    switcherRow->addWidget(transition);

    // Two compact downstream key cards expose only the live-critical settings.
    auto* keyers = makeModule("green");
    auto* keyersCol = new QVBoxLayout(keyers);
    keyersCol->setContentsMargins(11, 8, 11, 9);
    keyersCol->setSpacing(6);
    keyersCol->addWidget(
        makeSectionTitle(QStringLiteral("DOWNSTREAM KEYS"), QStringLiteral("OVERLAYS")));
    auto* cards = new QHBoxLayout;
    cards->setSpacing(7);
    for (int k = 0; k < kDskCount; ++k) {
        auto* card = new QFrame;
        card->setObjectName(QStringLiteral("keyerCard"));
        auto* cardCol = new QVBoxLayout(card);
        cardCol->setContentsMargins(7, 6, 7, 7);
        cardCol->setSpacing(5);
        auto* title = new QLabel(QStringLiteral("DSK %1").arg(k + 1));
        title->setObjectName(QStringLiteral("keyerTitle"));
        cardCol->addWidget(title);

        auto* settings = new QHBoxLayout;
        dskSrc_[k] = new QComboBox;
        dskSrc_[k]->setToolTip(QStringLiteral("Key source"));
        for (int i = 0; i < inputNames.size(); ++i)
            dskSrc_[k]->addItem(QStringLiteral("%1 · %2")
                                    .arg(i + 1, 2, 10, QLatin1Char('0'))
                                    .arg(displayName(inputNames[i]).left(12)));
        settings->addWidget(dskSrc_[k], 1);
        dskFade_[k] = new QSpinBox;
        dskFade_[k]->setRange(1, 600);
        dskFade_[k]->setValue(30);
        dskFade_[k]->setSuffix(QStringLiteral(" fr"));
        dskFade_[k]->setToolTip(QStringLiteral("Key fade duration"));
        settings->addWidget(dskFade_[k]);
        cardCol->addLayout(settings);

        dskBtns_[k] = new QPushButton(QStringLiteral("TAKE"));
        dskBtns_[k]->setObjectName(QStringLiteral("keyButton"));
        dskBtns_[k]->setProperty("active", false);
        connect(dskBtns_[k], &QPushButton::clicked, &bridge_,
                [&bridge = bridge_, k] { bridge.dskToggle(k); });
        cardCol->addWidget(dskBtns_[k]);
        cards->addWidget(card);

        connect(dskSrc_[k], &QComboBox::currentIndexChanged, &bridge_,
                [&bridge = bridge_, k](int source) {
                    bridge.setDskSource(k, source);
                });
        connect(dskFade_[k], &QSpinBox::valueChanged, &bridge_,
                [&bridge = bridge_, k](int ticks) {
                    bridge.setDskFade(k, ticks);
                });
    }
    keyersCol->addLayout(cards);
    keyersCol->addStretch(1);
    keyers->setMinimumWidth(350);
    keyers->setMaximumWidth(395);
    switcherRow->addWidget(keyers);

    workSplitter->addWidget(controlSurface);
    workSplitter->setStretchFactor(0, 5);
    workSplitter->setStretchFactor(1, 4);
    workSplitter->setSizes({470, 365});
    root->addWidget(workSplitter, 1);

    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("statusLine"));
    // Runtime diagnostics can be several thousand pixels wide. They should
    // clip at the window edge, never raise the minimum width of the console.
    status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(status_);
    connect(&bridge_, &EngineBridge::statusText, status_, &QLabel::setText);
    connect(&bridge_, &EngineBridge::inputNamesChanged, this,
            &MainWindow::onInputNames);
    connect(&bridge_, &EngineBridge::stateChanged, this, &MainWindow::onState);

    connect(transType_, &QComboBox::currentIndexChanged, this,
            &MainWindow::pushTransition);
    connect(transDur_, &QSpinBox::valueChanged, this,
            &MainWindow::pushTransition);
    if (initial && initial->transType >= 0 &&
        initial->transType < transType_->count())
        transType_->setCurrentIndex(initial->transType);
    pushTransition();

    new QShortcut(QKeySequence(Qt::Key_Space), this, [this] { bridge_.cut(); });
    new QShortcut(QKeySequence(Qt::Key_Return), this,
                  [this] { bridge_.autoTrans(); });
    new QShortcut(QKeySequence(Qt::Key_F), this,
                  [this] { bridge_.fadeToBlack(); });
    new QShortcut(QKeySequence(Qt::Key_D), this,
                  [this] { bridge_.dskToggle(0); });
    new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_D), this,
                  [this] { bridge_.dskToggle(1); });
    for (int i = 0; i < inputNames.size() && i < 9; ++i) {
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
            if (d.source >= 0 && d.source < dskSrc_[k]->count())
                dskSrc_[k]->setCurrentIndex(d.source);
            dskFade_[k]->setValue(d.fadeDurTicks);
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

    refreshMediaControls();
    refreshRecordingState();
    auto* mediaTimer = new QTimer(this);
    mediaTimer->setInterval(250);
    connect(mediaTimer, &QTimer::timeout, this, [this] {
        refreshMediaControls();
        refreshRecordingState();
    });
    mediaTimer->start();

    setCentralWidget(central);
    onInputNames(inputNames);
    refreshBusReadouts();
    resize(1440, 900);
}

ShowFile::State MainWindow::collectState() const {
    ShowFile::State state = baseState_;
    state.cfg.show.width = outputResolution_->currentData().toInt();
    state.cfg.show.height =
        outputResolution_->currentData(Qt::UserRole + 1).toInt();
    state.cfg.show.fpsN = outputFrameRate_->currentData().toLongLong();
    state.cfg.show.fpsD =
        outputFrameRate_->currentData(Qt::UserRole + 1).toLongLong();
    state.cfg.show.colorimetry =
        VideoFormatDesc::colorimetryForHeight(state.cfg.show.height);
    state.cfg.inputs.clear();
    for (int i = 0; i < bridge_.inputCount(); ++i) {
        // The engine knows each input's true type; re-deriving it from the
        // ref would misfile OMT discovery names (no scheme) as NDI.
        InputSpec spec{InputSpec::Type(bridge_.inputType(i)),
                       bridge_.inputRef(i).toStdString(),
                       bridge_.inputSyncFrames(i)};
        const auto media = bridge_.mediaState(i);
        if (media.available) {
            spec.mediaPlaying = media.playing;
            spec.mediaLoop = media.loop;
            spec.mediaPlaylist = bridge_.mediaPlaylistItems(i);
            if (!spec.mediaPlaylist.empty())
                spec.ref = spec.mediaPlaylist.front().path;
        }
        state.cfg.inputs.push_back(std::move(spec));
    }
    state.program = lastProgram_ < 0 ? 0 : lastProgram_;
    state.preview = lastPreview_ < 0 ? 1 : lastPreview_;
    state.transType = transType_->currentIndex();
    state.transDurTicks = transDur_->value();
    for (int k = 0; k < kDskCount; ++k)
        state.dsk[k] = {dskSrc_[k]->currentIndex(), dskFade_[k]->value(),
                        lastDskOn_[k]};
    state.cfg.masterAudioDelayMs = bridge_.masterDelayMs();
    state.chans.clear();
    for (int i = 0; i < bridge_.inputCount(); ++i)
        state.chans.push_back({bridge_.audioGain(i), bridge_.audioMute(i),
                               bridge_.audioSolo(i),
                               bridge_.audioInputDelayMs(i)});
    return state;
}

void MainWindow::saveShow() {
    if (!showFile_) return;
    ShowFile::State state = collectState();
    if (everSaved_ && state == lastSaved_) return;
    showFile_->save(state);
    lastSaved_ = std::move(state);
    everSaved_ = true;
}

void MainWindow::pushTransition() {
    bridge_.setTransition(transType_->currentIndex(), transDur_->value(), 0.02f);
}

void MainWindow::refreshOutputFormatState() {
    const int width = outputResolution_->currentData().toInt();
    const int height =
        outputResolution_->currentData(Qt::UserRole + 1).toInt();
    const qlonglong fpsN = outputFrameRate_->currentData().toLongLong();
    const qlonglong fpsD =
        outputFrameRate_->currentData(Qt::UserRole + 1).toLongLong();
    const bool pending = width != activeOutput_.width ||
                         height != activeOutput_.height ||
                         fpsN != activeOutput_.fpsN || fpsD != activeOutput_.fpsD;
    outputFormatState_->setText(pending ? QStringLiteral("RESTART TO APPLY")
                                        : QStringLiteral("ACTIVE"));
    outputFormatState_->setToolTip(
        pending
            ? QStringLiteral("Saved for the next start. The engine is currently "
                             "running at %1 × %2, %3p.")
                  .arg(activeOutput_.width)
                  .arg(activeOutput_.height)
                  .arg(decimalFrameRate(activeOutput_.fpsN, activeOutput_.fpsD))
            : QStringLiteral("This is the format currently used by the engine."));
    setVisualState(outputFormatState_, "state", pending ? "pending" : "active");
}

void MainWindow::refreshMediaControls() {
    auto formatTime = [](int64_t milliseconds) {
        if (milliseconds <= 0) return QStringLiteral("00:00");
        const int64_t seconds = milliseconds / 1000;
        return QStringLiteral("%1:%2")
            .arg(seconds / 60, 2, 10, QLatin1Char('0'))
            .arg(seconds % 60, 2, 10, QLatin1Char('0'));
    };

    for (int i = 0; i < int(mediaRows_.size()); ++i) {
        auto& row = mediaRows_[size_t(i)];
        const auto state = bridge_.mediaState(i);
        const bool available = state.available;
        const QString currentRef =
            QString::fromStdString(state.currentRef);
        row.name->setText(
            available
                ? QStringLiteral("INPUT %1   ·   CLIP %2/%3   ·   %4")
                      .arg(i + 1, 2, 10, QLatin1Char('0'))
                      .arg(state.playlistIndex + 1)
                      .arg(state.playlistSize)
                      .arg(displayName(currentRef))
                : QStringLiteral("INPUT %1   ·   NO MEDIA PLAYLIST")
                      .arg(i + 1, 2, 10, QLatin1Char('0')));
        row.time->setText(
            available
                ? QStringLiteral("%1 / %2   ·   %3 → %4   ·   ×%5")
                      .arg(formatTime(state.positionMs),
                           formatTime(state.durationMs),
                           formatTime(state.trimInMs),
                           state.trimOutMs > 0
                               ? formatTime(state.trimOutMs)
                               : QStringLiteral("END"),
                           QString::number(
                               double(state.speedPermille) / 1000.0,
                               'f', 2))
                : QStringLiteral("— / —"));
        for (auto* button : {row.play, row.previous, row.restart, row.next,
                             row.loop})
            button->setEnabled(available);
        row.previous->setEnabled(available && state.playlistSize > 1);
        row.next->setEnabled(available && state.playlistSize > 1);

        row.play->blockSignals(true);
        row.play->setChecked(available && state.playing);
        row.play->setText(state.playing ? QStringLiteral("PAUSE")
                                        : state.atEnd ? QStringLiteral("REPLAY")
                                                      : QStringLiteral("PLAY"));
        row.play->blockSignals(false);
        row.loop->blockSignals(true);
        row.loop->setChecked(available && state.loop);
        row.loop->blockSignals(false);
        setVisualState(row.play, "active", available && state.playing);
        setVisualState(row.loop, "active", available && state.loop);
    }
}

void MainWindow::refreshRecordingState() {
    const auto state = bridge_.recordingState();
    const auto cleanState = bridge_.cleanRecordingState();
    const auto refresh = [](const Engine::RecordingState& current,
                            QPushButton* button, QLabel* label,
                            const QString& idleText) {
        if (current.pending) return;  // retain STARTING/STOPPING feedback
        button->blockSignals(true);
        button->setChecked(current.active);
        button->setText(current.active ? QStringLiteral("■  STOP")
                                       : idleText);
        button->blockSignals(false);

        if (current.error) {
            label->setText(QStringLiteral("RECORD ERROR"));
            label->setToolTip(QString::fromStdString(current.path));
            setVisualState(label, "state", "error");
        } else if (current.active) {
            label->setText(QStringLiteral("REC  %1").arg(current.frames));
            label->setToolTip(QString::fromStdString(current.path));
            setVisualState(label, "state", "recording");
        } else {
            label->setText(QStringLiteral("IDLE"));
            label->setToolTip({});
            setVisualState(label, "state", "idle");
        }
    };
    refresh(state, recordBtn_, recordState_, QStringLiteral("●  RECORD"));
    refresh(cleanState, cleanRecordBtn_, cleanRecordState_,
            QStringLiteral("●  CLEAN"));
}

void MainWindow::refreshBusReadouts() {
    auto nameAt = [this](int index) {
        return index >= 0 && index < inputNames_.size()
                   ? inputNames_[index].left(24)
                   : QStringLiteral("—");
    };
    programReadout_->setText(
        QStringLiteral("PGM  %1  %2")
            .arg(lastProgram_ >= 0 ? lastProgram_ + 1 : 0, 2, 10, QLatin1Char('0'))
            .arg(nameAt(lastProgram_).toUpper()));
    previewReadout_->setText(
        QStringLiteral("PVW  %1  %2")
            .arg(lastPreview_ >= 0 ? lastPreview_ + 1 : 0, 2, 10, QLatin1Char('0'))
            .arg(nameAt(lastPreview_).toUpper()));
}

void MainWindow::onInputNames(const QStringList& refs) {
    inputNames_.clear();
    for (const QString& ref : refs) inputNames_ << displayName(ref);
    multiview_->setInputNames(inputNames_);

    for (int i = 0; i < inputNames_.size(); ++i) {
        const QString name = inputNames_[i];
        const QString text =
            QStringLiteral("%1\n%2")
                .arg(i + 1, 2, 10, QLatin1Char('0'))
                .arg(name.left(14).toUpper());
        if (i < int(pgmBtns_.size())) {
            pgmBtns_[size_t(i)]->setText(text);
            pgmBtns_[size_t(i)]->setToolTip(
                QStringLiteral("Send %1 to PROGRAM").arg(name));
        }
        if (i < int(pvwBtns_.size())) {
            pvwBtns_[size_t(i)]->setText(text);
            pvwBtns_[size_t(i)]->setToolTip(
                QStringLiteral("Send %1 to PREVIEW").arg(name));
        }
        for (int k = 0; k < kDskCount; ++k)
            if (i < dskSrc_[k]->count())
                dskSrc_[k]->setItemText(
                    i, QStringLiteral("%1 · %2")
                           .arg(i + 1, 2, 10, QLatin1Char('0'))
                           .arg(name.left(12)));
    }
    refreshBusReadouts();
    refreshMediaControls();
}

void MainWindow::onState(int program, int preview, bool inTransition, bool ftb,
                         bool dsk1, bool dsk2) {
    if (program != lastProgram_ || preview != lastPreview_) {
        for (int i = 0; i < int(pgmBtns_.size()); ++i)
            setVisualState(pgmBtns_[size_t(i)], "tally",
                           i == program ? "program" : "idle");
        for (int i = 0; i < int(pvwBtns_.size()); ++i)
            setVisualState(pvwBtns_[size_t(i)], "tally",
                           i == preview ? "preview" : "idle");
        lastProgram_ = program;
        lastPreview_ = preview;
        refreshBusReadouts();
    }

    setVisualState(autoBtn_, "active", inTransition);
    if (ftb != lastFtb_) {
        setVisualState(ftbBtn_, "active", ftb);
        ftbBtn_->setText(ftb ? QStringLiteral("BLACK ON AIR")
                             : QStringLiteral("FADE TO BLACK"));
        lastFtb_ = ftb;
    }

    const bool dskOn[kDskCount] = {dsk1, dsk2};
    for (int k = 0; k < kDskCount; ++k) {
        if (dskOn[k] == lastDskOn_[k]) continue;
        setVisualState(dskBtns_[k], "active", dskOn[k]);
        dskBtns_[k]->setText(dskOn[k] ? QStringLiteral("ON AIR")
                                      : QStringLiteral("TAKE"));
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
