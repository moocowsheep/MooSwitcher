#pragma once
#include <QString>

#include <vector>

#include "engine/Engine.h"

namespace moo::ui {

// Show-file persistence: everything needed to restore a show after a
// restart, in a human-readable INI the operator can copy between machines.
// Load builds the EngineConfig before engine start; the control-surface and
// mixer parts are applied after. The GUI saves on a debounced timer.
class ShowFile {
public:
    struct ChannelState {
        float gain = 1.f;
        bool mute = false;
        bool solo = false;
        int delayMs = 0;

        bool operator==(const ChannelState&) const = default;
    };

    struct DskState {
        int source = 0;
        int fadeDurTicks = 30;
        bool on = false;
        bool tie = false;
        bool audioFollow = false;

        bool operator==(const DskState&) const = default;
    };

    struct State {
        EngineConfig cfg;
        int program = 0;
        int preview = 1;
        int transType = 0;
        int transDurTicks = 30;
        std::vector<ChannelState> chans;
        DskState dsk[kDskCount];

        bool operator==(const State& o) const {
            return program == o.program && preview == o.preview &&
                   transType == o.transType && transDurTicks == o.transDurTicks &&
                   chans == o.chans && dsk[0] == o.dsk[0] && dsk[1] == o.dsk[1] &&
                   cfgEquals(cfg, o.cfg);
        }

    private:
        static bool cfgEquals(const EngineConfig& a, const EngineConfig& b);
    };

    explicit ShowFile(QString path);  // empty = default config location

    const QString& path() const { return path_; }
    bool exists() const;

    // Overwrites cfg/control fields with the stored show; returns false when
    // no file exists (state left untouched).
    bool load(State& state) const;
    void save(const State& state) const;

private:
    QString path_;
};

}  // namespace moo::ui
