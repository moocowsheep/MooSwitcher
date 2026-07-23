/* MooSwitcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * Additional permission under GNU GPL version 3 section 7: you may link
 * MooSwitcher against the proprietary NDI SDK, the NVIDIA CUDA / Video
 * Codec SDK runtime (CUDA, NVENC, NVDEC), and the OMT (libomt / libvmx)
 * runtime, and distribute the combined work. See LICENSE.md for the full
 * exception text. */

#include "ui/MixerPanel.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "media/StillImage.h"

namespace moo::ui {

namespace {

QString shortSourceName(QString ref) {
    if (ref.startsWith(QStringLiteral("srt://"), Qt::CaseInsensitive))
        ref = QStringLiteral("SRT · ") + ref.mid(6);
    else if (ref.startsWith(QStringLiteral("omt://"), Qt::CaseInsensitive))
        ref = QStringLiteral("OMT · ") + ref.mid(6);
    else if (QFileInfo(ref).isAbsolute())
        ref = (media::isStillImagePath(ref.toStdString())
                   ? QStringLiteral("STILL · ")
                   : QStringLiteral("MEDIA · ")) +
              QFileInfo(ref).fileName();
    return ref.isEmpty() ? QStringLiteral("BLACK") : ref;
}

// Pick an NDI/OMT source from discovery or type an NDI name substring /
// srt:// / omt:// URL. Discovery rows carry their type in item data --
// OMT names have no scheme, so text alone cannot distinguish them.
class SourcePickerDialog : public QDialog {
public:
    enum {
        kTypeRole = Qt::UserRole,
        kNameRole = Qt::UserRole + 1,
        kInRole = Qt::UserRole + 2,
        kOutRole = Qt::UserRole + 3,
        kSpeedRole = Qt::UserRole + 4,
    };

    SourcePickerDialog(EngineBridge& bridge, int input, QWidget* parent)
        : QDialog(parent), bridge_(bridge) {
        setWindowTitle(QStringLiteral("Configure input %1").arg(input + 1));
        auto* col = new QVBoxLayout(this);
        col->setContentsMargins(16, 16, 16, 16);
        col->setSpacing(10);

        auto* title = new QLabel(QStringLiteral("INPUT %1 SOURCE")
                                     .arg(input + 1, 2, 10, QLatin1Char('0')));
        title->setObjectName(QStringLiteral("sectionTitle"));
        col->addWidget(title);
        auto* hint = new QLabel(QStringLiteral(
            "Choose a source, enter a URL, build a video playlist, or load a still image."));
        hint->setObjectName(QStringLiteral("sectionHint"));
        col->addWidget(hint);

        list_ = new QListWidget;
        refresh();
        col->addWidget(list_, 1);

        auto* manualRow = new QHBoxLayout;
        manual_ = new QLineEdit;
        manual_->setPlaceholderText(
            QStringLiteral("srt://host:9710?mode=caller&latency=120000"));
        manualRow->addWidget(manual_, 1);
        auto* refreshBtn = new QPushButton(QStringLiteral("REFRESH"));
        refreshBtn->setObjectName(QStringLiteral("actionButton"));
        connect(refreshBtn, &QPushButton::clicked, this, [this] { refresh(); });
        manualRow->addWidget(refreshBtn);
        auto* mediaBtn = new QPushButton(QStringLiteral("ADD CLIPS"));
        mediaBtn->setObjectName(QStringLiteral("actionButton"));
        connect(mediaBtn, &QPushButton::clicked, this,
                [this] { addMedia(); });
        manualRow->addWidget(mediaBtn);
        auto* stillBtn = new QPushButton(QStringLiteral("ADD STILL"));
        stillBtn->setObjectName(QStringLiteral("actionButton"));
        connect(stillBtn, &QPushButton::clicked, this,
                [this] { addStill(); });
        manualRow->addWidget(stillBtn);
        col->addLayout(manualRow);
        connect(manual_, &QLineEdit::textEdited, this,
                [this] {
                    mediaChosen_ = false;
                    stillChosen_ = false;
                });

        auto* playlistLabel = new QLabel(QStringLiteral(
            "VIDEO PLAYLIST   ·   clips advance top-to-bottom"));
        playlistLabel->setObjectName(QStringLiteral("eyebrow"));
        col->addWidget(playlistLabel);
        playlist_ = new QListWidget;
        playlist_->setMaximumHeight(105);
        for (const auto& item : bridge.mediaPlaylistItems(input))
            addPlaylistItem(item);
        col->addWidget(playlist_);

        auto* trimRow = new QHBoxLayout;
        auto* trimLabel = new QLabel(QStringLiteral("TRIM"));
        trimLabel->setObjectName(QStringLiteral("eyebrow"));
        trimRow->addWidget(trimLabel);
        trimRow->addWidget(new QLabel(QStringLiteral("IN")));
        trimIn_ = new QSpinBox;
        trimIn_->setRange(0, 86'400'000);
        trimIn_->setSpecialValueText(QStringLiteral("START"));
        trimIn_->setSuffix(QStringLiteral(" ms"));
        trimIn_->setToolTip(
            QStringLiteral("Inclusive in point, relative to source start"));
        trimRow->addWidget(trimIn_, 1);
        trimRow->addWidget(new QLabel(QStringLiteral("OUT")));
        trimOut_ = new QSpinBox;
        trimOut_->setRange(0, 86'400'000);
        trimOut_->setSpecialValueText(QStringLiteral("END"));
        trimOut_->setSuffix(QStringLiteral(" ms"));
        trimOut_->setToolTip(
            QStringLiteral("Exclusive out point; END plays through EOF"));
        trimRow->addWidget(trimOut_, 1);
        trimRow->addWidget(new QLabel(QStringLiteral("SPEED")));
        speed_ = new QSpinBox;
        speed_->setRange(25, 400);
        speed_->setValue(100);
        speed_->setSuffix(QStringLiteral(" %"));
        speed_->setToolTip(QStringLiteral(
            "Playback speed; audio tempo is adjusted without changing pitch"));
        trimRow->addWidget(speed_, 1);
        col->addLayout(trimRow);
        connect(playlist_, &QListWidget::currentRowChanged, this,
                [this](int row) { loadTrim(row); });
        connect(trimIn_, &QSpinBox::valueChanged, this,
                [this] { storeTrim(); });
        connect(trimOut_, &QSpinBox::valueChanged, this,
                [this] { storeTrim(); });
        connect(speed_, &QSpinBox::valueChanged, this,
                [this] { storeTrim(); });

        auto* playlistButtons = new QHBoxLayout;
        const auto addPlaylistButton = [&](const QString& text, auto action) {
            auto* button = new QPushButton(text);
            button->setObjectName(QStringLiteral("actionButton"));
            connect(button, &QPushButton::clicked, this, action);
            playlistButtons->addWidget(button);
        };
        addPlaylistButton(QStringLiteral("REMOVE"), [this] {
            if (auto* item = playlist_->takeItem(playlist_->currentRow())) {
                delete item;
                mediaChosen_ = playlist_->count() > 0;
            }
        });
        addPlaylistButton(QStringLiteral("MOVE UP"),
                          [this] { moveMedia(-1); });
        addPlaylistButton(QStringLiteral("MOVE DOWN"),
                          [this] { moveMedia(1); });
        playlistButtons->addStretch(1);
        col->addLayout(playlistButtons);

        auto* syncRow = new QHBoxLayout;
        auto* syncLabel = new QLabel(QStringLiteral("FRAME SYNC"));
        syncLabel->setObjectName(QStringLiteral("eyebrow"));
        syncRow->addWidget(syncLabel);
        sync_ = new QComboBox;
        sync_->addItems({QStringLiteral("Off"),
                         QStringLiteral("Trim only · no delay"),
                         QStringLiteral("1 frame"), QStringLiteral("2 frames"),
                         QStringLiteral("3 frames"), QStringLiteral("4 frames")});
        sync_->setToolTip(QStringLiteral(
            "Re-time this input onto the output tick grid and auto-align its audio."));
        sync_->setCurrentIndex(bridge.inputSyncFrames(input) + 1);
        syncRow->addWidget(sync_, 1);
        col->addLayout(syncRow);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                             QDialogButtonBox::Cancel);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(list_, &QListWidget::itemDoubleClicked, this, &QDialog::accept);
        connect(list_, &QListWidget::itemClicked, this,
                [this] {
                    mediaChosen_ = false;
                    stillChosen_ = false;
                    manual_->clear();
                });
        if (playlist_->count() > 0) playlist_->setCurrentRow(0);
        col->addWidget(buttons);
        resize(620, 590);
    }

    QString chosen() const {
        if (mediaChosen_ && playlist_->count() > 0)
            return playlist_->item(0)->data(kNameRole).toString();
        const QString manual = manual_->text().trimmed();
        if (!manual.isEmpty()) return manual;
        if (auto* item = list_->currentItem())
            return item->data(kNameRole).toString();
        return {};
    }

    // -1 = infer from the ref (manual entry); explicit for discovery rows.
    int chosenType() const {
        if (mediaChosen_ && playlist_->count() > 0) return 3;
        if (stillChosen_) return 4;
        if (!manual_->text().trimmed().isEmpty()) return -1;
        if (auto* item = list_->currentItem())
            return item->data(kTypeRole).toInt();
        return -1;
    }

    int syncFrames() const { return sync_->currentIndex() - 1; }

    std::vector<media::PlaylistItem> chosenPlaylist() const {
        std::vector<media::PlaylistItem> items;
        if (!mediaChosen_) return items;
        items.reserve(size_t(playlist_->count()));
        for (int i = 0; i < playlist_->count(); ++i) {
            const auto* row = playlist_->item(i);
            items.emplace_back(row->data(kNameRole).toString().toStdString(),
                               row->data(kInRole).toLongLong(),
                               row->data(kOutRole).toLongLong(),
                               row->data(kSpeedRole).toInt());
        }
        return items;
    }

private:
    void addSource(const QString& name, int type, const QString& badge) {
        auto* item = new QListWidgetItem(
            QStringLiteral("%1     %2").arg(badge, name));
        item->setData(kTypeRole, type);
        item->setData(kNameRole, name);
        list_->addItem(item);
    }

    static QString formatTrimTime(int64_t milliseconds, bool end = false) {
        if (end && milliseconds == 0) return QStringLiteral("END");
        const int64_t seconds = milliseconds / 1000;
        return QStringLiteral("%1:%2.%3")
            .arg(seconds / 60, 2, 10, QLatin1Char('0'))
            .arg(seconds % 60, 2, 10, QLatin1Char('0'))
            .arg(milliseconds % 1000, 3, 10, QLatin1Char('0'));
    }

    void refreshPlaylistItem(QListWidgetItem* item) {
        if (!item) return;
        const QString path = item->data(kNameRole).toString();
        const int64_t inMs = item->data(kInRole).toLongLong();
        const int64_t outMs = item->data(kOutRole).toLongLong();
        const int speedPermille = item->data(kSpeedRole).toInt();
        item->setText(
            QStringLiteral("%1   ·   %2 → %3   ·   ×%4   ·   %5")
                .arg(QFileInfo(path).fileName(), formatTrimTime(inMs),
                     formatTrimTime(outMs, true),
                     QString::number(double(speedPermille) / 1000.0, 'f', 2),
                     QFileInfo(path).absolutePath()));
        item->setToolTip(path);
    }

    void addPlaylistItem(const media::PlaylistItem& clip) {
        auto* item = new QListWidgetItem;
        item->setData(kNameRole, QString::fromStdString(clip.path));
        item->setData(kInRole, qlonglong(clip.inMs));
        item->setData(kOutRole, qlonglong(clip.outMs));
        item->setData(kSpeedRole, clip.speedPermille);
        refreshPlaylistItem(item);
        playlist_->addItem(item);
    }

    void loadTrim(int row) {
        const bool valid = row >= 0 && row < playlist_->count();
        trimIn_->setEnabled(valid);
        trimOut_->setEnabled(valid);
        speed_->setEnabled(valid);
        trimIn_->blockSignals(true);
        trimOut_->blockSignals(true);
        speed_->blockSignals(true);
        trimIn_->setValue(
            valid ? int(playlist_->item(row)->data(kInRole).toLongLong()) : 0);
        trimOut_->setValue(
            valid ? int(playlist_->item(row)->data(kOutRole).toLongLong()) : 0);
        speed_->setValue(
            valid ? playlist_->item(row)->data(kSpeedRole).toInt() / 10 : 100);
        trimIn_->blockSignals(false);
        trimOut_->blockSignals(false);
        speed_->blockSignals(false);
    }

    void storeTrim() {
        auto* item = playlist_->currentItem();
        if (!item) return;
        int inMs = trimIn_->value();
        int outMs = trimOut_->value();
        if (outMs > 0 && outMs <= inMs) {
            outMs = inMs < trimOut_->maximum() ? inMs + 1 : 0;
            trimOut_->blockSignals(true);
            trimOut_->setValue(outMs);
            trimOut_->blockSignals(false);
        }
        item->setData(kInRole, qlonglong(inMs));
        item->setData(kOutRole, qlonglong(outMs));
        item->setData(kSpeedRole, speed_->value() * 10);
        refreshPlaylistItem(item);
        mediaChosen_ = true;
    }

    void addMedia() {
        const QStringList paths = QFileDialog::getOpenFileNames(
            this, QStringLiteral("Add video clips"), {},
            QStringLiteral(
                "Video files (*.mkv *.mp4 *.mov *.m4v *.ts);;All files (*)"));
        if (paths.isEmpty()) return;
        for (const QString& path : paths)
            addPlaylistItem(media::PlaylistItem{path.toStdString()});
        mediaChosen_ = true;
        stillChosen_ = false;
        manual_->clear();
        list_->clearSelection();
        sync_->setCurrentIndex(0);
        playlist_->setCurrentRow(playlist_->count() - 1);
    }

    void addStill() {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Load still image"), {},
            QStringLiteral(
                "Still images (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff "
                "*.tga *.exr);;All files (*)"));
        if (path.isEmpty()) return;
        manual_->setText(path);
        mediaChosen_ = false;
        stillChosen_ = true;
        list_->clearSelection();
        sync_->setCurrentIndex(0);
    }

    void moveMedia(int direction) {
        const int row = playlist_->currentRow();
        const int target = row + direction;
        if (row < 0 || target < 0 || target >= playlist_->count()) return;
        auto* item = playlist_->takeItem(row);
        playlist_->insertItem(target, item);
        playlist_->setCurrentRow(target);
        mediaChosen_ = true;
    }

    void refresh() {
        list_->clear();
        for (const QString& name : bridge_.ndiSourceNames())
            addSource(name, 0, QStringLiteral("NDI"));
        for (const QString& name : bridge_.omtSourceNames())
            addSource(name, 2, QStringLiteral("OMT"));
        if (list_->count() == 0) {
            auto* empty = new QListWidgetItem(
                QStringLiteral("No discovered sources · manual entry is still available"));
            empty->setFlags(Qt::NoItemFlags);
            list_->addItem(empty);
        }
    }

    EngineBridge& bridge_;
    QListWidget* list_ = nullptr;
    QListWidget* playlist_ = nullptr;
    QLineEdit* manual_ = nullptr;
    QComboBox* sync_ = nullptr;
    QSpinBox* trimIn_ = nullptr;
    QSpinBox* trimOut_ = nullptr;
    QSpinBox* speed_ = nullptr;
    bool mediaChosen_ = false;
    bool stillChosen_ = false;
};

float dbFor(float linear) {
    return linear > 1e-6f ? 20.f * std::log10(linear) : -120.f;
}

float yFor(float db, int height) {
    const float t = std::clamp((db + 60.f) / 60.f, 0.f, 1.f);
    return float(height) * (1.f - t);
}

QString gainText(int tenthsDb) {
    if (tenthsDb <= -600) return QStringLiteral("−∞ dB");
    return QStringLiteral("%1 dB").arg(double(tenthsDb) / 10.0, 0, 'f', 1);
}

}  // namespace

MeterWidget::MeterWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(30, 78);
    setMaximumWidth(30);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
}

void MeterWidget::setLevels(float left, float right) {
    dispL_ = std::max(left, dispL_ * 0.80f);
    dispR_ = std::max(right, dispR_ * 0.80f);
    if (left >= holdL_ || ++holdAgeL_ > 45) {
        holdL_ = left;
        holdAgeL_ = 0;
    }
    if (right >= holdR_ || ++holdAgeR_ > 45) {
        holdR_ = right;
        holdAgeR_ = 0;
    }
    update();
}

void MeterWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(8, 9, 11));

    constexpr int kSegments = 20;
    constexpr int kGap = 2;
    const int barWidth = (width() - kGap - 4) / 2;
    const int innerHeight = height() - 4;
    const float segmentHeight = float(innerHeight) / float(kSegments);

    const auto drawChannel = [&](int x, float displayed, float hold) {
        const float displayedDb = dbFor(displayed);
        for (int segment = 0; segment < kSegments; ++segment) {
            const float lowDb = -60.f + float(segment) * 3.f;
            const int y = height() - 2 -
                          int(std::round(float(segment + 1) * segmentHeight));
            QRect segmentRect(x, y + 1, barWidth,
                              std::max(1, int(segmentHeight) - 1));
            QColor color(27, 30, 34);
            if (displayedDb >= lowDb) {
                color = lowDb >= -6.f    ? QColor(240, 54, 62)
                        : lowDb >= -18.f ? QColor(240, 183, 60)
                                         : QColor(47, 197, 110);
            }
            painter.fillRect(segmentRect, color);
        }
        const float holdDb = dbFor(hold);
        if (holdDb > -60.f) {
            const int y = std::clamp(int(yFor(holdDb, height() - 4)) + 2,
                                     1, height() - 2);
            painter.fillRect(x, y, barWidth, 1, QColor(238, 244, 248));
        }
    };

    drawChannel(2, dispL_, holdL_);
    drawChannel(2 + barWidth + kGap, dispR_, holdR_);
    painter.setPen(QPen(QColor(56, 61, 67), 1));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
}

MixerPanel::MixerPanel(EngineBridge& bridge, const QStringList& names,
                       QWidget* parent)
    : QWidget(parent), bridge_(bridge) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(11, 9, 11, 10);
    root->setSpacing(8);

    auto* header = new QHBoxLayout;
    auto* title = new QLabel(QStringLiteral("AUDIO MIXER   ·   INPUTS  /  MASTER"));
    title->setObjectName(QStringLiteral("sectionTitle"));
    header->addWidget(title);
    header->addStretch(1);
    auto* hint = new QLabel(QStringLiteral("CLICK A SOURCE NAME TO PATCH INPUT OR SET FRAME SYNC"));
    hint->setObjectName(QStringLiteral("sectionHint"));
    header->addWidget(hint);
    root->addLayout(header);

    auto* stripCanvas = new QWidget;
    auto* stripRow = new QHBoxLayout(stripCanvas);
    stripRow->setContentsMargins(0, 0, 0, 0);
    stripRow->setSpacing(9);

    for (int i = 0; i < names.size(); ++i) {
        auto* strip = new QFrame;
        strip->setObjectName(QStringLiteral("channelStrip"));
        strip->setFixedWidth(139);
        auto* col = new QVBoxLayout(strip);
        col->setContentsMargins(8, 7, 8, 8);
        col->setSpacing(5);

        auto* index = new QLabel(
            QStringLiteral("INPUT %1").arg(i + 1, 2, 10, QLatin1Char('0')));
        index->setObjectName(QStringLiteral("channelIndex"));
        index->setAlignment(Qt::AlignCenter);
        col->addWidget(index);

        auto* nameButton = new QPushButton(shortSourceName(names[i]).left(16));
        nameButton->setObjectName(QStringLiteral("channelName"));
        nameButton->setToolTip(
            QStringLiteral("Configure source and frame sync for input %1").arg(i + 1));
        nameBtns_.push_back(nameButton);
        connect(nameButton, &QPushButton::clicked, this, [this, i] {
            SourcePickerDialog dialog(bridge_, i, this);
            if (dialog.exec() != QDialog::Accepted) return;
            // No source picked = keep the current one. This path must retain
            // the engine's explicit type for scheme-less OMT discovery names.
            const bool keep = dialog.chosen().isEmpty();
            const QString ref = keep ? bridge_.inputRef(i) : dialog.chosen();
            if (ref.isEmpty()) return;
            const auto playlist = dialog.chosenPlaylist();
            if (!playlist.empty()) {
                if (playlist == bridge_.mediaPlaylistItems(i) &&
                    dialog.syncFrames() == bridge_.inputSyncFrames(i))
                    return;
                bridge_.replaceMediaPlaylist(i, playlist,
                                             dialog.syncFrames());
                return;
            }
            // A frame-sync-only edit must preserve an existing playlist.
            if (keep && bridge_.inputType(i) == int(InputSpec::Type::Media)) {
                const auto existing = bridge_.mediaPlaylistItems(i);
                if (!existing.empty()) {
                    if (dialog.syncFrames() != bridge_.inputSyncFrames(i))
                        bridge_.replaceMediaPlaylist(
                            i, existing, dialog.syncFrames());
                    return;
                }
            }
            if (ref == bridge_.inputRef(i) &&
                dialog.syncFrames() == bridge_.inputSyncFrames(i))
                return;
            bridge_.replaceInput(i, ref, dialog.syncFrames(),
                                 keep ? bridge_.inputType(i)
                                      : dialog.chosenType());
        });
        col->addWidget(nameButton);

        auto* meterFader = new QHBoxLayout;
        meterFader->setSpacing(5);
        auto* scale = new QVBoxLayout;
        scale->setContentsMargins(0, 1, 0, 1);
        for (const QString& tick : {QStringLiteral("0"), QStringLiteral("−6"),
                                    QStringLiteral("−18"), QStringLiteral("−36"),
                                    QStringLiteral("−60")}) {
            auto* label = new QLabel(tick);
            label->setObjectName(QStringLiteral("meterScale"));
            label->setAlignment(Qt::AlignRight);
            scale->addWidget(label);
            if (tick != QStringLiteral("−60")) scale->addStretch(1);
        }
        meterFader->addLayout(scale);

        auto* meter = new MeterWidget;
        meters_.push_back(meter);
        meterFader->addWidget(meter);

        auto* fader = new QSlider(Qt::Vertical);
        fader->setObjectName(QStringLiteral("audioFader"));
        fader->setRange(-600, 100);  // tenths of dB; bottom = off
        const float initialGain = bridge.audioGain(i);
        const int initialDb = initialGain <= 0.001f
                                  ? -600
                                  : int(std::lround(200.f * std::log10(initialGain)));
        fader->setValue(initialDb);
        fader->setMinimumHeight(78);
        meterFader->addWidget(fader, 0, Qt::AlignHCenter);
        col->addLayout(meterFader, 1);

        auto* gain = new QLabel(gainText(initialDb));
        gain->setObjectName(QStringLiteral("gainReadout"));
        gain->setAlignment(Qt::AlignCenter);
        col->addWidget(gain);
        connect(fader, &QSlider::valueChanged, &bridge,
                [&bridge, i, gain](int value) {
                    const float linear =
                        value <= -600
                            ? 0.f
                            : std::pow(10.f, float(value) / 10.f / 20.f);
                    bridge.setAudioGain(i, linear);
                    gain->setText(gainText(value));
                });

        auto* toggles = new QHBoxLayout;
        toggles->setSpacing(5);
        auto* mute = new QPushButton(QStringLiteral("MUTE"));
        mute->setObjectName(QStringLiteral("mixerToggle"));
        mute->setProperty("kind", "mute");
        mute->setCheckable(true);
        mute->setChecked(bridge.audioMute(i));
        connect(mute, &QPushButton::toggled, &bridge,
                [&bridge, i](bool on) { bridge.setAudioMute(i, on); });
        toggles->addWidget(mute, 1);
        auto* solo = new QPushButton(QStringLiteral("SOLO"));
        solo->setObjectName(QStringLiteral("mixerToggle"));
        solo->setProperty("kind", "solo");
        solo->setCheckable(true);
        solo->setChecked(bridge.audioSolo(i));
        connect(solo, &QPushButton::toggled, &bridge,
                [&bridge, i](bool on) { bridge.setAudioSolo(i, on); });
        toggles->addWidget(solo, 1);
        col->addLayout(toggles);

        auto* delayRow = new QHBoxLayout;
        auto* delayLabel = new QLabel(QStringLiteral("DELAY"));
        delayLabel->setObjectName(QStringLiteral("channelIndex"));
        delayRow->addWidget(delayLabel);
        auto* delay = new QSpinBox;
        delay->setRange(0, 500);
        delay->setValue(bridge.audioInputDelayMs(i));
        delay->setSuffix(QStringLiteral(" ms"));
        delay->setToolTip(QStringLiteral("Manual audio delay trim"));
        connect(delay, &QSpinBox::valueChanged, &bridge,
                [&bridge, i](int milliseconds) {
                    bridge.setAudioDelayMs(i, milliseconds);
                });
        delayRow->addWidget(delay, 1);
        col->addLayout(delayRow);

        auto* trim = new QLabel;
        trim->setObjectName(QStringLiteral("trimReadout"));
        trim->setAlignment(Qt::AlignCenter);
        trim->setToolTip(QStringLiteral(
            "Frame-sync auto A/V trim applied on top of manual delay"));
        trimLabels_.push_back(trim);
        col->addWidget(trim);

        stripRow->addWidget(strip);
    }

    // Master strip: post-limiter meter plus the global A/V calibration delay.
    auto* master = new QFrame;
    master->setObjectName(QStringLiteral("channelStrip"));
    master->setProperty("master", true);
    master->setFixedWidth(125);
    auto* masterCol = new QVBoxLayout(master);
    masterCol->setContentsMargins(8, 7, 8, 8);
    masterCol->setSpacing(6);
    auto* masterIndex = new QLabel(QStringLiteral("PROGRAM"));
    masterIndex->setObjectName(QStringLiteral("channelIndex"));
    masterIndex->setAlignment(Qt::AlignCenter);
    masterCol->addWidget(masterIndex);
    auto* masterTitle = new QLabel(QStringLiteral("MASTER"));
    masterTitle->setObjectName(QStringLiteral("sectionTitle"));
    masterTitle->setAlignment(Qt::AlignCenter);
    masterCol->addWidget(masterTitle);
    masterMeter_ = new MeterWidget;
    masterCol->addWidget(masterMeter_, 1, Qt::AlignHCenter);
    auto* masterDelayLabel = new QLabel(QStringLiteral("A/V CALIBRATION"));
    masterDelayLabel->setObjectName(QStringLiteral("channelIndex"));
    masterDelayLabel->setAlignment(Qt::AlignCenter);
    masterCol->addWidget(masterDelayLabel);
    auto* masterDelay = new QSpinBox;
    masterDelay->setRange(0, 200);
    masterDelay->setValue(bridge.masterDelayMs());
    masterDelay->setSuffix(QStringLiteral(" ms"));
    masterDelay->setToolTip(QStringLiteral("Master output audio delay"));
    connect(masterDelay, &QSpinBox::valueChanged, &bridge,
            [&bridge](int milliseconds) {
                bridge.setMasterDelayMs(milliseconds);
            });
    masterCol->addWidget(masterDelay);
    stripRow->addWidget(master);
    stripRow->addStretch(1);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(stripCanvas);
    root->addWidget(scroll, 1);
}

void MixerPanel::onLevels(QList<float> levels) {
    const int count = int(meters_.size());
    if (levels.size() < count * 2 + 2) return;
    for (int i = 0; i < count; ++i)
        meters_[size_t(i)]->setLevels(levels[i * 2], levels[i * 2 + 1]);
    masterMeter_->setLevels(levels[count * 2], levels[count * 2 + 1]);
    for (int i = 0; i < int(trimLabels_.size()); ++i) {
        const int milliseconds = bridge_.audioAutoTrimMs(i);
        const QString text = milliseconds > 0
                                 ? QStringLiteral("AUTO +%1 ms").arg(milliseconds)
                                 : QStringLiteral("AUTO —");
        if (trimLabels_[size_t(i)]->text() != text)
            trimLabels_[size_t(i)]->setText(text);
    }
}

void MixerPanel::onInputNames(QStringList refs) {
    for (int i = 0; i < int(nameBtns_.size()) && i < refs.size(); ++i) {
        const QString name = shortSourceName(refs[i]);
        nameBtns_[size_t(i)]->setText(name.left(16));
        nameBtns_[size_t(i)]->setToolTip(
            QStringLiteral("Configure %1 and frame sync").arg(name));
    }
}

}  // namespace moo::ui
