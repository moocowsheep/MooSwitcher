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

#pragma once
#include <QImage>
#include <QObject>
#include <QTimer>

#include <vector>

#include "engine/Engine.h"

namespace moo::ui {

// GUI-side adapter: commands in (queued to the engine), multiview frames and
// state out (30 Hz poll). No engine callbacks ever run on the GUI thread.
class EngineBridge : public QObject {
    Q_OBJECT
public:
    explicit EngineBridge(Engine& engine, QObject* parent = nullptr);

public slots:
    void setProgram(int src) { engine_.post({Command::Type::SetProgram, src, 0, 0.f}); }
    void setPreview(int src) { engine_.post({Command::Type::SetPreview, src, 0, 0.f}); }
    void cut() { engine_.post({Command::Type::Cut, 0, 0, 0.f}); }
    void autoTrans() { engine_.post({Command::Type::Auto, 0, 0, 0.f}); }
    void fadeToBlack() { engine_.post({Command::Type::FadeToBlack, 0, 0, 0.f}); }
    void tbarBegin() { engine_.post({Command::Type::TbarBegin, 0, 0, 0.f}); }
    void tbarSet(float pos) { engine_.post({Command::Type::TbarSet, 0, 0, pos}); }
    void tbarEnd() { engine_.post({Command::Type::TbarEnd, 0, 0, 0.f}); }
    void setTransition(int type, int durationTicks, float softness) {
        engine_.post({Command::Type::SetTransition, type, durationTicks, softness});
    }
    void dskToggle(int k) { engine_.post({Command::Type::DskToggle, k, 0, 0.f}); }
    void setDskSource(int k, int src) {
        engine_.post({Command::Type::SetDskSource, k, src, 0.f});
    }
    void setDskFade(int k, int ticks) {
        engine_.post({Command::Type::SetDskFade, k, ticks, 0.f});
    }
    void setDskTie(int k, bool on) {
        engine_.post({Command::Type::SetDskTie, k, on ? 1 : 0, 0.f});
    }
    void setDskAudioFollow(int k, bool on) {
        engine_.post({Command::Type::SetDskAudioFollow, k, on ? 1 : 0, 0.f});
    }
    void setMediaPlaying(int input, bool playing) {
        engine_.post(
            {Command::Type::MediaSetPlaying, input, playing ? 1 : 0, 0.f});
    }
    void restartMedia(int input) {
        engine_.post({Command::Type::MediaRestart, input, 0, 0.f});
    }
    void setMediaLoop(int input, bool loop) {
        engine_.post({Command::Type::MediaSetLoop, input, loop ? 1 : 0, 0.f});
    }
    void stepMedia(int input, int direction) {
        engine_.post({Command::Type::MediaStep, input, direction, 0.f});
    }
    void startRecording(QString path);
    void stopRecording() { engine_.requestRecording({}); }
    void startCleanRecording(QString path);
    void stopCleanRecording() { engine_.requestCleanRecording({}); }

    // Audio mixer controls: straight to the mixer atomics (thread-safe).
    void setAudioGain(int input, float linearGain);
    void setAudioMute(int input, bool on);
    void setAudioSolo(int input, bool on);
    void setAudioDelayMs(int input, int ms);
    void setMasterDelayMs(int ms);

    // Source picker: type -1 = infer from the ref (srt://->SRT, omt://->OMT,
    // anything else = NDI name substring); 0/1/2/3/4 force
    // Ndi/Srt/Omt/Media/Still for discovery and local-file choices.
    // syncFrames: -1 off, 0 measure-only (auto A/V trim), 1..4 buffered.
    // Static stills always force sync off. Takes effect at the next render
    // tick.
    void replaceInput(int input, QString ref, int syncFrames, int type = -1);
    void replaceMediaPlaylist(
        int input, std::vector<media::PlaylistItem> items, int syncFrames);

public:
    bool audioAvailable() const { return engine_.audio() != nullptr; }
    int audioInputDelayMs(int input) const;
    int masterDelayMs() const;
    float audioGain(int input) const;
    bool audioMute(int input) const;
    bool audioSolo(int input) const;
    QStringList ndiSourceNames() const;
    QStringList omtSourceNames() const;  // empty when built without OMT
    QString inputRef(int input) const;
    // InputSpec::Type as int (0 NDI, 1 SRT, 2 OMT, 3 media, 4 still).
    int inputType(int input) const;
    IInputSource::MediaState mediaState(int input) const {
        return engine_.inputMediaState(input);
    }
    std::vector<media::PlaylistItem> mediaPlaylistItems(int input) const {
        return engine_.inputMediaPlaylist(input);
    }
    Engine::RecordingState recordingState() const {
        return engine_.recordingState();
    }
    Engine::RecordingState cleanRecordingState() const {
        return engine_.cleanRecordingState();
    }
    int inputSyncFrames(int input) const { return engine_.inputSyncFrames(input); }
    int audioAutoTrimMs(int input) const;  // applied frame-sync trim readout
    int inputCount() const { return engine_.inputCount(); }
    VideoFormatDesc outputFormat() const { return engine_.outputFormat(); }

signals:
    void multiviewFrame(QImage frame);
    void statusText(QString text);
    void stateChanged(int program, int preview, bool inTransition, bool ftb,
                      bool dsk1, bool dsk2);
    void dskOptionsChanged(bool tie1, bool tie2, bool afv1, bool afv2);
    void audioLevels(QList<float> lr);  // per input L,R ... then master L,R
    void inputNamesChanged(QStringList refs);
    void healthChanged(QStringList problems);  // empty = all good

private:
    void poll();

    Engine& engine_;
    QTimer timer_;
    std::vector<uint8_t> buf_;
    uint64_t seq_ = 0;
    QStringList lastRefs_;
    QStringList lastProblems_;
};

}  // namespace moo::ui
