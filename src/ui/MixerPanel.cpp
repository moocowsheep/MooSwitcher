#include "ui/MixerPanel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace moo::ui {

namespace {

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
    : QWidget(parent) {
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
        auto* col = makeStrip(
            QStringLiteral("%1 %2").arg(i + 1).arg(names[i].left(10)));

        auto* mid = new QHBoxLayout;
        auto* meter = new MeterWidget;
        meters_.push_back(meter);
        mid->addWidget(meter);

        auto* fader = new QSlider(Qt::Vertical);
        fader->setRange(-600, 100);  // tenths of dB; bottom = off
        fader->setValue(0);
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
        mute->setStyleSheet(kSmallBtn);
        connect(mute, &QPushButton::toggled, &bridge,
                [&bridge, i](bool on) { bridge.setAudioMute(i, on); });
        btns->addWidget(mute);
        auto* solo = new QPushButton(QStringLiteral("S"));
        solo->setCheckable(true);
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
}

}  // namespace moo::ui
