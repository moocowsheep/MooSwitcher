/* Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

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
