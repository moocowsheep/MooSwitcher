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
#include <string>
#include <thread>
#include <vector>

#include "ctl/ControlProtocol.h"

namespace moo {
class Engine;
}

namespace moo::ctl {

// TCP remote-control listener (docs/remote-control.md): accepts line
// commands, applies them to the engine, and pushes one-line JSON state
// events to subscribed clients whenever the state changes (~33 Hz poll).
// Single poll()-driven thread owns all sockets. Qt-free; used by both the
// GUI and moo-headless. A failed bind logs and leaves the server inert --
// remote control must never take the show down.
//
// Lifetime: the poll thread snapshots engine state every iteration, so the
// server MUST be destroyed before Engine::stop() tears down inputs/audio.
class ControlServer {
public:
    ControlServer(Engine& engine, int port);
    ~ControlServer();

    bool listening() const { return listenFd_ >= 0; }
    int port() const { return port_; }

private:
    struct Client {
        int fd = -1;
        bool subscribed = false;
        std::string in;   // partial line accumulation
        std::string out;  // unwritten bytes (slow reader)
    };

    void run(std::stop_token st);
    void apply(const Request& r, Client& c);
    Snapshot snapshot() const;
    void send(Client& c, const std::string& line);  // appends '\n'
    void sendError(Client& c, const std::string& msg);
    std::string defaultRecordPath(bool clean) const;

    Engine& engine_;
    int port_ = 0;
    int listenFd_ = -1;
    int wakeFd_ = -1;  // eventfd: ~ControlServer interrupts the poll
    std::vector<Client> clients_;
    std::string lastState_;
    std::jthread thread_;
};

}  // namespace moo::ctl
