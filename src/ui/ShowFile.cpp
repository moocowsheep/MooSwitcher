#include "ui/ShowFile.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>

namespace moo::ui {

bool ShowFile::State::cfgEquals(const EngineConfig& a, const EngineConfig& b) {
    if (a.inputs.size() != b.inputs.size()) return false;
    for (size_t i = 0; i < a.inputs.size(); ++i)
        if (a.inputs[i].type != b.inputs[i].type ||
            a.inputs[i].ref != b.inputs[i].ref ||
            a.inputs[i].syncFrames != b.inputs[i].syncFrames ||
            a.inputs[i].mediaLoop != b.inputs[i].mediaLoop)
            return false;
    return a.show == b.show && a.ndiOut == b.ndiOut &&
           a.ndiOutName == b.ndiOutName && a.srtUrl == b.srtUrl &&
           a.srtBitrateKbps == b.srtBitrateKbps &&
           a.recordBitrateKbps == b.recordBitrateKbps && a.audio == b.audio &&
           a.masterAudioDelayMs == b.masterAudioDelayMs;
}

ShowFile::ShowFile(QString path) : path_(std::move(path)) {
    if (path_.isEmpty())
        path_ = QStandardPaths::writableLocation(
                    QStandardPaths::AppConfigLocation) +
                QStringLiteral("/show.ini");
    QDir().mkpath(QFileInfo(path_).absolutePath());
}

bool ShowFile::exists() const { return QFileInfo::exists(path_); }

bool ShowFile::load(State& st) const {
    if (!exists()) return false;
    QSettings s(path_, QSettings::IniFormat);

    s.beginGroup(QStringLiteral("show"));
    st.cfg.show.width = s.value("width", st.cfg.show.width).toInt();
    st.cfg.show.height = s.value("height", st.cfg.show.height).toInt();
    const qlonglong fpsN =
        s.value("fpsN", qlonglong(st.cfg.show.fpsN)).toLongLong();
    const qlonglong fpsD =
        s.value("fpsD", qlonglong(st.cfg.show.fpsD)).toLongLong();
    if (fpsN > 0 && fpsD > 0) {
        st.cfg.show.fpsN = fpsN;
        st.cfg.show.fpsD = fpsD;
    }
    st.cfg.ndiOut = s.value("ndiOut", st.cfg.ndiOut).toBool();
    st.cfg.ndiOutName =
        s.value("ndiOutName", QString::fromStdString(st.cfg.ndiOutName))
            .toString()
            .toStdString();
    st.cfg.srtUrl = s.value("srtOut", QString::fromStdString(st.cfg.srtUrl))
                        .toString()
                        .toStdString();
    st.cfg.srtBitrateKbps =
        s.value("srtBitrateKbps", st.cfg.srtBitrateKbps).toInt();
    st.cfg.recordBitrateKbps =
        s.value("recordBitrateKbps", st.cfg.recordBitrateKbps).toInt();
    st.cfg.audio = s.value("audio", st.cfg.audio).toBool();
    st.cfg.masterAudioDelayMs =
        s.value("masterDelayMs", st.cfg.masterAudioDelayMs).toInt();
    s.endGroup();

    const int n = s.beginReadArray(QStringLiteral("inputs"));
    if (n > 0) st.cfg.inputs.clear();
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        InputSpec spec;
        const QString type = s.value("type").toString();
        spec.type = type == QStringLiteral("srt")   ? InputSpec::Type::Srt
                    : type == QStringLiteral("omt") ? InputSpec::Type::Omt
                    : type == QStringLiteral("media")
                        ? InputSpec::Type::Media
                                                    : InputSpec::Type::Ndi;
        spec.ref = s.value("ref").toString().toStdString();
        // Absent in v1 show files -> stays off (-1).
        spec.syncFrames = s.value("framesync", spec.syncFrames).toInt();
        if (spec.syncFrames < -1 || spec.syncFrames > 4) spec.syncFrames = -1;
        spec.mediaLoop = s.value("mediaLoop", spec.mediaLoop).toBool();
        st.cfg.inputs.push_back(std::move(spec));
    }
    s.endArray();

    s.beginGroup(QStringLiteral("switcher"));
    st.program = s.value("program", st.program).toInt();
    st.preview = s.value("preview", st.preview).toInt();
    st.transType = s.value("transType", st.transType).toInt();
    st.transDurTicks = s.value("transDurTicks", st.transDurTicks).toInt();
    s.endGroup();

    st.chans.assign(st.cfg.inputs.size(), ChannelState{});
    const int c = s.beginReadArray(QStringLiteral("audioChannels"));
    for (int i = 0; i < c && i < int(st.chans.size()); ++i) {
        s.setArrayIndex(i);
        auto& ch = st.chans[size_t(i)];
        ch.gain = s.value("gain", ch.gain).toFloat();
        ch.mute = s.value("mute", ch.mute).toBool();
        ch.solo = s.value("solo", ch.solo).toBool();
        ch.delayMs = s.value("delayMs", ch.delayMs).toInt();
    }
    s.endArray();

    // Absent in pre-DSK show files -> defaults (off).
    const int nd = s.beginReadArray(QStringLiteral("dsk"));
    for (int i = 0; i < nd && i < kDskCount; ++i) {
        s.setArrayIndex(i);
        auto& d = st.dsk[i];
        d.source = std::clamp(s.value("source", d.source).toInt(), 0,
                              std::max(0, int(st.cfg.inputs.size()) - 1));
        d.fadeDurTicks =
            std::clamp(s.value("fadeDurTicks", d.fadeDurTicks).toInt(), 1, 600);
        d.on = s.value("on", d.on).toBool();
    }
    s.endArray();
    return true;
}

void ShowFile::save(const State& st) const {
    QSettings s(path_, QSettings::IniFormat);

    s.beginGroup(QStringLiteral("show"));
    s.setValue("width", st.cfg.show.width);
    s.setValue("height", st.cfg.show.height);
    s.setValue("fpsN", qlonglong(st.cfg.show.fpsN));
    s.setValue("fpsD", qlonglong(st.cfg.show.fpsD));
    s.setValue("ndiOut", st.cfg.ndiOut);
    s.setValue("ndiOutName", QString::fromStdString(st.cfg.ndiOutName));
    s.setValue("srtOut", QString::fromStdString(st.cfg.srtUrl));
    s.setValue("srtBitrateKbps", st.cfg.srtBitrateKbps);
    s.setValue("recordBitrateKbps", st.cfg.recordBitrateKbps);
    s.setValue("audio", st.cfg.audio);
    s.setValue("masterDelayMs", st.cfg.masterAudioDelayMs);
    s.endGroup();

    s.beginWriteArray(QStringLiteral("inputs"), int(st.cfg.inputs.size()));
    for (int i = 0; i < int(st.cfg.inputs.size()); ++i) {
        s.setArrayIndex(i);
        const auto& spec = st.cfg.inputs[size_t(i)];
        s.setValue(
            "type",
            spec.type == InputSpec::Type::Srt     ? QStringLiteral("srt")
            : spec.type == InputSpec::Type::Omt   ? QStringLiteral("omt")
            : spec.type == InputSpec::Type::Media ? QStringLiteral("media")
                                                  : QStringLiteral("ndi"));
        s.setValue("ref", QString::fromStdString(spec.ref));
        s.setValue("framesync", spec.syncFrames);
        if (spec.type == InputSpec::Type::Media) {
            s.setValue("mediaLoop", spec.mediaLoop);
        } else {
            s.remove("mediaLoop");
        }
        // A restored show cues media from its start and plays; pause is an
        // ephemeral transport state, not a startup mode.
        s.remove("mediaPlaying");
    }
    s.endArray();

    s.beginGroup(QStringLiteral("switcher"));
    s.setValue("program", st.program);
    s.setValue("preview", st.preview);
    s.setValue("transType", st.transType);
    s.setValue("transDurTicks", st.transDurTicks);
    s.endGroup();

    s.beginWriteArray(QStringLiteral("audioChannels"), int(st.chans.size()));
    for (int i = 0; i < int(st.chans.size()); ++i) {
        s.setArrayIndex(i);
        const auto& ch = st.chans[size_t(i)];
        s.setValue("gain", double(ch.gain));
        s.setValue("mute", ch.mute);
        s.setValue("solo", ch.solo);
        s.setValue("delayMs", ch.delayMs);
    }
    s.endArray();

    s.beginWriteArray(QStringLiteral("dsk"), kDskCount);
    for (int i = 0; i < kDskCount; ++i) {
        s.setArrayIndex(i);
        s.setValue("source", st.dsk[i].source);
        s.setValue("fadeDurTicks", st.dsk[i].fadeDurTicks);
        s.setValue("on", st.dsk[i].on);
    }
    s.endArray();
}

}  // namespace moo::ui
