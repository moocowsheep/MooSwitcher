#include "ui/MixerPanel.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace moo::ui {

namespace {

// Pick an NDI/OMT source from discovery or type an NDI name substring /
// srt:// / omt:// URL. Discovery rows carry their type in item data --
// OMT names have no scheme, so text alone cannot distinguish them.
class SourcePickerDialog : public QDialog {
public:
    enum { kTypeRole = Qt::UserRole, kNameRole = Qt::UserRole + 1 };

    SourcePickerDialog(EngineBridge& bridge, int input, QWidget* parent)
        : QDialog(parent), bridge_(bridge) {
        setWindowTitle(QStringLiteral("Input %1 source").arg(input + 1));
        auto* col = new QVBoxLayout(this);

        list_ = new QListWidget;
        refresh();
        col->addWidget(list_, 1);

        auto* refreshBtn = new QPushButton(QStringLiteral("Refresh"));
        connect(refreshBtn, &QPushButton::clicked, this,
                [this] { refresh(); });
        col->addWidget(refreshBtn);

        col->addWidget(new QLabel(
            QStringLiteral("...or NDI name substring / srt:// / omt:// URL:")));
        manual_ = new QLineEdit;
        manual_->setPlaceholderText(
            QStringLiteral("srt://host:9710?mode=caller&latency=120000"));
        col->addWidget(manual_);

        auto* fsRow = new QHBoxLayout;
        fsRow->addWidget(new QLabel(QStringLiteral("Frame sync:")));
        sync_ = new QComboBox;
        sync_->addItems({QStringLiteral("Off"),
                         QStringLiteral("Trim only (no delay)"),
                         QStringLiteral("1 frame"), QStringLiteral("2 frames"),
                         QStringLiteral("3 frames"),
                         QStringLiteral("4 frames")});
        sync_->setToolTip(QStringLiteral(
            "Re-time this input onto the output tick grid and auto-align "
            "its audio. Trim only fixes A/V phase without adding latency; "
            "N frames also absorbs bursty delivery."));
        sync_->setCurrentIndex(bridge.inputSyncFrames(input) + 1);
        fsRow->addWidget(sync_, 1);
        col->addLayout(fsRow);

        auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok |
                                        QDialogButtonBox::Cancel);
        connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(list_, &QListWidget::itemDoubleClicked, this,
                &QDialog::accept);
        col->addWidget(bb);
        resize(420, 320);
    }

    QString chosen() const {
        const QString manual = manual_->text().trimmed();
        if (!manual.isEmpty()) return manual;
        if (auto* item = list_->currentItem())
            return item->data(kNameRole).toString();
        return {};
    }
    // -1 = infer from the ref (manual entry); explicit for discovery rows.
    int chosenType() const {
        if (!manual_->text().trimmed().isEmpty()) return -1;
        if (auto* item = list_->currentItem())
            return item->data(kTypeRole).toInt();
        return -1;
    }
    int syncFrames() const { return sync_->currentIndex() - 1; }

private:
    void addSource(const QString& name, int type, const char* badge) {
        auto* item = new QListWidgetItem(
            QStringLiteral("%1  %2").arg(QLatin1String(badge), name));
        item->setData(kTypeRole, type);
        item->setData(kNameRole, name);
        list_->addItem(item);
    }

    void refresh() {
        list_->clear();
        for (const QString& n : bridge_.ndiSourceNames())
            addSource(n, 0, "NDI");  // InputSpec::Type::Ndi
        for (const QString& n : bridge_.omtSourceNames())
            addSource(n, 2, "OMT");  // InputSpec::Type::Omt
    }

    EngineBridge& bridge_;
    QListWidget* list_ = nullptr;
    QLineEdit* manual_ = nullptr;
    QComboBox* sync_ = nullptr;
};

float dbFor(float lin) { return lin > 1e-6f ? 20.f * std::log10(lin) : -120.f; }

// Meter/fader share the -60..0 dB scale.
float yFor(float db, int h) {
    const float t = std::clamp((db + 60.f) / 60.f, 0.f, 1.f);
    return float(h) * (1.f - t);
}

const char* kSmallBtn =
    "QPushButton { font-weight: bold; min-width: 22px; max-width: 30px; }"
    "QPushButton:checked { background: #b08018; color: white; }";

}  // namespace

MeterWidget::MeterWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(18, 100);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
}

void MeterWidget::setLevels(float l, float r) {
    dispL_ = std::max(l, dispL_ * 0.80f);
    dispR_ = std::max(r, dispR_ * 0.80f);
    if (l >= holdL_ || ++holdAgeL_ > 45) {  // ~1.5 s hold at 30 Hz
        holdL_ = l;
        holdAgeL_ = 0;
    }
    if (r >= holdR_ || ++holdAgeR_ > 45) {
        holdR_ = r;
        holdAgeR_ = 0;
    }
    update();
}

void MeterWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const int h = height();
    const int barW = (width() - 2) / 2;

    p.fillRect(rect(), QColor(24, 24, 24));
    const auto drawBar = [&](int x, float disp, float hold) {
        const float db = dbFor(disp);
        const int top = int(yFor(db, h));
        // Zone slices: green to -18, yellow to -6, red above.
        const int yYellow = int(yFor(-18.f, h));
        const int yRed = int(yFor(-6.f, h));
        if (top < h)
            p.fillRect(x, std::max(top, yYellow), barW, h - std::max(top, yYellow),
                       QColor(46, 160, 67));
        if (top < yYellow)
            p.fillRect(x, std::max(top, yRed), barW, yYellow - std::max(top, yRed),
                       QColor(212, 167, 44));
        if (top < yRed) p.fillRect(x, top, barW, yRed - top, QColor(207, 34, 46));
        const float hdb = dbFor(hold);
        if (hdb > -60.f)
            p.fillRect(x, int(yFor(hdb, h)), barW, 2, QColor(230, 230, 230));
    };
    drawBar(0, dispL_, holdL_);
    drawBar(barW + 2, dispR_, holdR_);
    // -18/-6 scale ticks.
    p.setPen(QColor(90, 90, 90));
    p.drawLine(0, int(yFor(-18.f, h)), width(), int(yFor(-18.f, h)));
    p.drawLine(0, int(yFor(-6.f, h)), width(), int(yFor(-6.f, h)));
}

MixerPanel::MixerPanel(EngineBridge& bridge, const QStringList& names,
                       QWidget* parent)
    : QWidget(parent), bridge_(bridge) {
    auto* row = new QHBoxLayout(this);
    row->setSpacing(10);

    auto makeStrip = [&](const QString& label) {
        auto* col = new QVBoxLayout;
        auto* tag = new QLabel(label);
        tag->setStyleSheet(QStringLiteral("font-weight: bold;"));
        tag->setMaximumWidth(96);
        col->addWidget(tag, 0, Qt::AlignHCenter);
        return col;
    };

    for (int i = 0; i < names.size(); ++i) {
        auto* col = new QVBoxLayout;
        auto* nameBtn = new QPushButton(
            QStringLiteral("%1 %2").arg(i + 1).arg(names[i].left(10)));
        nameBtn->setFlat(true);
        nameBtn->setStyleSheet(QStringLiteral("font-weight: bold;"));
        nameBtn->setToolTip(QStringLiteral("Pick source for input %1").arg(i + 1));
        nameBtn->setMaximumWidth(110);
        connect(nameBtn, &QPushButton::clicked, this, [this, i] {
            SourcePickerDialog dlg(bridge_, i, this);
            if (dlg.exec() != QDialog::Accepted) return;
            // No source picked = keep the current one (frame-sync-only
            // change still goes through the replace path); keeping it must
            // also keep its TYPE -- an OMT discovery name would otherwise
            // re-resolve as an NDI substring.
            const bool keep = dlg.chosen().isEmpty();
            const QString ref = keep ? bridge_.inputRef(i) : dlg.chosen();
            if (ref.isEmpty()) return;
            if (ref == bridge_.inputRef(i) &&
                dlg.syncFrames() == bridge_.inputSyncFrames(i))
                return;  // nothing changed
            bridge_.replaceInput(i, ref, dlg.syncFrames(),
                                 keep ? bridge_.inputType(i) : dlg.chosenType());
        });
        nameBtns_.push_back(nameBtn);
        col->addWidget(nameBtn, 0, Qt::AlignHCenter);

        auto* mid = new QHBoxLayout;
        auto* meter = new MeterWidget;
        meters_.push_back(meter);
        mid->addWidget(meter);

        auto* fader = new QSlider(Qt::Vertical);
        fader->setRange(-600, 100);  // tenths of dB; bottom = off
        const float g0 = bridge.audioGain(i);
        fader->setValue(g0 <= 0.001f
                            ? -600
                            : int(std::lround(200.f * std::log10(g0))));
        fader->setMinimumHeight(110);
        connect(fader, &QSlider::valueChanged, &bridge, [&bridge, i](int v) {
            const float lin =
                v <= -600 ? 0.f : std::pow(10.f, float(v) / 10.f / 20.f);
            bridge.setAudioGain(i, lin);
        });
        mid->addWidget(fader);
        col->addLayout(mid, 1);

        auto* btns = new QHBoxLayout;
        auto* mute = new QPushButton(QStringLiteral("M"));
        mute->setCheckable(true);
        mute->setChecked(bridge.audioMute(i));
        mute->setStyleSheet(kSmallBtn);
        connect(mute, &QPushButton::toggled, &bridge,
                [&bridge, i](bool on) { bridge.setAudioMute(i, on); });
        btns->addWidget(mute);
        auto* solo = new QPushButton(QStringLiteral("S"));
        solo->setCheckable(true);
        solo->setChecked(bridge.audioSolo(i));
        solo->setStyleSheet(kSmallBtn);
        connect(solo, &QPushButton::toggled, &bridge,
                [&bridge, i](bool on) { bridge.setAudioSolo(i, on); });
        btns->addWidget(solo);
        col->addLayout(btns);

        auto* delay = new QSpinBox;
        delay->setRange(0, 500);
        delay->setValue(bridge.audioInputDelayMs(i));
        delay->setSuffix(QStringLiteral(" ms"));
        delay->setToolTip(QStringLiteral("Audio delay trim for this input"));
        connect(delay, &QSpinBox::valueChanged, &bridge,
                [&bridge, i](int ms) { bridge.setAudioDelayMs(i, ms); });
        col->addWidget(delay);

        auto* trim = new QLabel;
        trim->setStyleSheet(QStringLiteral("color: #888; font-size: 10px;"));
        trim->setToolTip(QStringLiteral(
            "Frame-sync auto A/V trim applied on top of the manual delay"));
        trimLabels_.push_back(trim);
        col->addWidget(trim, 0, Qt::AlignHCenter);

        row->addLayout(col);
    }

    // Master strip: post-limiter meter + the A/V calibration delay.
    auto* col = makeStrip(QStringLiteral("MASTER"));
    masterMeter_ = new MeterWidget;
    col->addWidget(masterMeter_, 1, Qt::AlignHCenter);
    auto* mdelay = new QSpinBox;
    mdelay->setRange(0, 200);
    mdelay->setValue(bridge.masterDelayMs());
    mdelay->setSuffix(QStringLiteral(" ms"));
    mdelay->setToolTip(
        QStringLiteral("Master audio delay (A/V sync calibration)"));
    connect(mdelay, &QSpinBox::valueChanged, &bridge,
            [&bridge](int ms) { bridge.setMasterDelayMs(ms); });
    col->addWidget(mdelay);
    row->addLayout(col);

    row->addStretch(1);
}

void MixerPanel::onLevels(QList<float> lr) {
    const int n = int(meters_.size());
    if (lr.size() < n * 2 + 2) return;
    for (int i = 0; i < n; ++i)
        meters_[size_t(i)]->setLevels(lr[i * 2], lr[i * 2 + 1]);
    masterMeter_->setLevels(lr[n * 2], lr[n * 2 + 1]);
    for (int i = 0; i < int(trimLabels_.size()); ++i) {
        const int ms = bridge_.audioAutoTrimMs(i);
        const QString t =
            ms > 0 ? QStringLiteral("auto %1 ms").arg(ms) : QString();
        if (trimLabels_[size_t(i)]->text() != t)
            trimLabels_[size_t(i)]->setText(t);
    }
}

void MixerPanel::onInputNames(QStringList refs) {
    for (int i = 0; i < int(nameBtns_.size()) && i < refs.size(); ++i) {
        QString n = refs[i];
        if (n.startsWith(QStringLiteral("srt://")))
            n = QStringLiteral("SRT ") + n.mid(6);
        nameBtns_[size_t(i)]->setText(
            QStringLiteral("%1 %2").arg(i + 1).arg(n.left(10)));
    }
}

}  // namespace moo::ui
