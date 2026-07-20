#include "ui/EngineBridge.h"

#include <QFileInfo>

#include "core/Stats.h"
#include "media/StillImage.h"

namespace moo::ui {

namespace {
audio::InputChannel* chan(Engine& e, int i) {
    auto* a = e.audio();
    return (a && i >= 0 && i < a->inputCount()) ? &a->channel(i) : nullptr;
}
}  // namespace

EngineBridge::EngineBridge(Engine& engine, QObject* parent)
    : QObject(parent), engine_(engine) {
    connect(&timer_, &QTimer::timeout, this, &EngineBridge::poll);
    timer_.start(33);
}

void EngineBridge::setAudioGain(int input, float linearGain) {
    if (auto* c = chan(engine_, input))
        c->gain.store(linearGain, std::memory_order_relaxed);
}

void EngineBridge::setAudioMute(int input, bool on) {
    if (auto* c = chan(engine_, input))
        c->mute.store(on, std::memory_order_relaxed);
}

void EngineBridge::setAudioSolo(int input, bool on) {
    if (auto* c = chan(engine_, input))
        c->solo.store(on, std::memory_order_relaxed);
}

void EngineBridge::setAudioDelayMs(int input, int ms) {
    if (auto* c = chan(engine_, input))
        c->delayMs.store(ms, std::memory_order_relaxed);
}

void EngineBridge::setMasterDelayMs(int ms) {
    if (auto* a = engine_.audio())
        a->masterDelayMs.store(ms, std::memory_order_relaxed);
}

int EngineBridge::audioInputDelayMs(int input) const {
    auto* c = chan(engine_, input);
    return c ? c->delayMs.load(std::memory_order_relaxed) : 0;
}

int EngineBridge::masterDelayMs() const {
    auto* a = engine_.audio();
    return a ? a->masterDelayMs.load(std::memory_order_relaxed) : 0;
}

float EngineBridge::audioGain(int input) const {
    auto* c = chan(engine_, input);
    return c ? c->gain.load(std::memory_order_relaxed) : 1.f;
}

bool EngineBridge::audioMute(int input) const {
    auto* c = chan(engine_, input);
    return c && c->mute.load(std::memory_order_relaxed);
}

bool EngineBridge::audioSolo(int input) const {
    auto* c = chan(engine_, input);
    return c && c->solo.load(std::memory_order_relaxed);
}

void EngineBridge::replaceInput(int input, QString ref, int syncFrames,
                                int type) {
    const std::string r = ref.toStdString();
    const auto t =
        type >= 0 && type <= 4 ? InputSpec::Type(type)
        : r.rfind("srt://", 0) == 0   ? InputSpec::Type::Srt
        : r.rfind("omt://", 0) == 0   ? InputSpec::Type::Omt
        : QFileInfo(ref).isFile()
            ? (media::isStillImagePath(r) ? InputSpec::Type::Still
                                          : InputSpec::Type::Media)
                                      : InputSpec::Type::Ndi;
    engine_.requestInputReplace(input, {t, r, syncFrames});
}

void EngineBridge::replaceMediaPlaylist(
    int input, std::vector<media::PlaylistItem> items, int syncFrames) {
    InputSpec spec;
    spec.type = InputSpec::Type::Media;
    spec.syncFrames = syncFrames;
    if (const auto current = engine_.inputMediaState(input);
        current.available) {
        spec.mediaPlaying = current.playing;
        spec.mediaLoop = current.loop;
    }
    for (auto& item : items) {
        if (item.path.empty()) continue;
        media::normalizePlaylistItem(item);
        spec.mediaPlaylist.push_back(std::move(item));
    }
    if (spec.mediaPlaylist.empty()) return;
    spec.ref = spec.mediaPlaylist.front().path;
    engine_.requestInputReplace(input, std::move(spec));
}

void EngineBridge::startRecording(QString path) {
    engine_.requestRecording(path.toStdString());
}

void EngineBridge::startCleanRecording(QString path) {
    engine_.requestCleanRecording(path.toStdString());
}

int EngineBridge::audioAutoTrimMs(int input) const {
    auto* c = chan(engine_, input);
    return c ? c->autoDelayFrames.load(std::memory_order_relaxed) *
                   1000 / audio::kSampleRate
             : 0;
}

QStringList EngineBridge::ndiSourceNames() const {
    QStringList out;
    for (const auto& s : engine_.ndiSources())
        out << QString::fromStdString(s.name);
    return out;
}

QStringList EngineBridge::omtSourceNames() const {
    QStringList out;
    for (const auto& s : engine_.omtSources())
        out << QString::fromStdString(s);
    return out;
}

QString EngineBridge::inputRef(int input) const {
    return QString::fromStdString(engine_.inputRef(input));
}

int EngineBridge::inputType(int input) const {
    return int(engine_.inputType(input));
}

void EngineBridge::poll() {
    int w = 0, h = 0;
    if (engine_.copyMultiview(buf_, seq_, w, h)) {
        // Deep copy: buf_ is reused on the next poll.
        QImage img(buf_.data(), w, h, w * 4, QImage::Format_RGBA8888);
        emit multiviewFrame(img.copy());
    }

    const auto st = engine_.uiState();
    emit stateChanged(st.program, st.preview, st.inTransition, st.ftbEngaged,
                      st.dskOn[0], st.dskOn[1]);
    emit dskOptionsChanged(st.dskTie[0], st.dskTie[1], st.dskAudioFollow[0],
                           st.dskAudioFollow[1]);

    QStringList refs;
    for (int i = 0; i < engine_.inputCount(); ++i)
        refs << QString::fromStdString(engine_.inputRef(i));
    if (refs != lastRefs_) {
        lastRefs_ = refs;
        emit inputNamesChanged(refs);
    }

    QStringList problems;
    for (int i = 0; i < engine_.inputCount(); ++i)
        if (!engine_.inputStatus(i).connected && !engine_.inputRef(i).empty())
            problems << QStringLiteral("IN%1 no signal").arg(i + 1);
    if (engine_.srtConfigured() && !engine_.srtConnected())
        problems << QStringLiteral("SRT out down (reconnecting)");
    if (problems != lastProblems_) {
        lastProblems_ = problems;
        emit healthChanged(problems);
    }

    if (auto* a = engine_.audio()) {
        QList<float> lv;
        lv.reserve(a->inputCount() * 2 + 2);
        for (int i = 0; i < a->inputCount(); ++i) {
            lv << a->channel(i).peakL.exchange(0.f, std::memory_order_relaxed)
               << a->channel(i).peakR.exchange(0.f, std::memory_order_relaxed);
        }
        lv << a->masterPeakL.exchange(0.f, std::memory_order_relaxed)
           << a->masterPeakR.exchange(0.f, std::memory_order_relaxed);
        emit audioLevels(lv);
    }

    QString status = QStringLiteral("ticks %1  skips %2  ndi-out %3")
                         .arg(engine_.renderedTicks())
                         .arg(engine_.skippedTicks())
                         .arg(engine_.ndiOutFrames());
    if (engine_.cleanNdiOutFrames())
        status += QStringLiteral("  clean-ndi %1")
                      .arg(engine_.cleanNdiOutFrames());
    if (auto* a = engine_.audio())
        status += QStringLiteral("  aud[sk %1 un %2]")
                      .arg(a->mixSkips())
                      .arg(a->underruns());
    for (int i = 0; i < engine_.inputCount(); ++i) {
        if (engine_.inputRef(i).empty()) continue;  // unassigned = quiet
        const auto s = engine_.inputStatus(i);
        status += QStringLiteral("   in%1: %2 %3x%4 f=%5 d=%6 r=%7")
                      .arg(i)
                      .arg(s.connected ? QStringLiteral("up") : QStringLiteral("down"))
                      .arg(s.desc.width)
                      .arg(s.desc.height)
                      .arg(s.frames)
                      .arg(s.drops)
                      .arg(Stats::counter("in" + std::to_string(i) + ".repeats")
                               .value());
    }
    emit statusText(status);
}

}  // namespace moo::ui
