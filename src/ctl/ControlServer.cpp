#include "ctl/ControlServer.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "core/Log.h"
#include "engine/Engine.h"

namespace moo::ctl {

namespace {
constexpr size_t kMaxClients = 16;
constexpr size_t kMaxLine = 4096;     // longest accepted request line
constexpr size_t kMaxOutBuf = 1 << 20;  // slow reader: drop past 1 MiB queued
constexpr int kPollMs = 30;           // state publish cadence

bool setNonBlock(int fd) {
    const int fl = fcntl(fd, F_GETFL, 0);
    return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}
}  // namespace

ControlServer::ControlServer(Engine& engine, int port)
    : engine_(engine), port_(port) {
    listenFd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listenFd_ < 0) {
        MOO_LOGE("control: socket: %s", strerror(errno));
        return;
    }
    const int one = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(uint16_t(port));
    if (bind(listenFd_, (const sockaddr*)&addr, sizeof addr) != 0 ||
        listen(listenFd_, 8) != 0 || !setNonBlock(listenFd_)) {
        MOO_LOGE("control: cannot listen on tcp/%d: %s (remote control off)",
                 port, strerror(errno));
        close(listenFd_);
        listenFd_ = -1;
        return;
    }
    wakeFd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
    MOO_LOGI("control: listening on tcp/%d", port);
}

ControlServer::~ControlServer() {
    if (thread_.joinable()) {
        thread_.request_stop();
        if (wakeFd_ >= 0) {
            const uint64_t one = 1;
            [[maybe_unused]] const ssize_t n = write(wakeFd_, &one, sizeof one);
        }
        thread_.join();
    }
    for (const auto& c : clients_) close(c.fd);
    if (wakeFd_ >= 0) close(wakeFd_);
    if (listenFd_ >= 0) close(listenFd_);
}

void ControlServer::send(Client& c, const std::string& line) {
    if (c.out.size() + line.size() > kMaxOutBuf) {
        c.out.clear();
        c.in = "\x01";  // poison: reader drops the client this iteration
        return;
    }
    c.out += line;
    c.out += '\n';
}

void ControlServer::sendError(Client& c, const std::string& msg) {
    send(c, "{\"event\":\"error\",\"message\":\"" + jsonEscape(msg) + "\"}");
}

std::string ControlServer::defaultRecordPath(bool clean) const {
    const char* home = getenv("HOME");
    std::string dir = home ? home : ".";
    struct stat st{};
    if (const std::string videos = dir + "/Videos";
        stat(videos.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
        dir = videos;
    char ts[32];
    const time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &local);
    return dir + (clean ? "/MooSwitcher-Clean-" : "/MooSwitcher-") + ts +
           ".mkv";
}

Snapshot ControlServer::snapshot() const {
    Snapshot s;
    const auto ui = engine_.uiState();
    s.program = ui.program;
    s.preview = ui.preview;
    const auto fmt = engine_.outputFormat();
    if (fmt.fpsD > 0) s.fps = double(fmt.fpsN) / double(fmt.fpsD);
    s.inTransition = ui.inTransition;
    s.ftb = ui.ftbEngaged;
    s.ftbLevel = ui.ftbLevel;
    s.transitionType = ui.transType;
    s.dsk.resize(kDskCount);
    for (int k = 0; k < kDskCount; ++k)
        s.dsk[size_t(k)] = {ui.dskOn[k], ui.dskLevel[k], ui.dskSrc[k]};
    auto rec = [](const Engine::RecordingState& r) {
        return RecordControlState{r.active, r.pending, r.error, r.frames,
                                  r.path};
    };
    s.record = rec(engine_.recordingState());
    s.cleanRecord = rec(engine_.cleanRecordingState());
    s.srtConfigured = engine_.srtConfigured();
    s.srtConnected = engine_.srtConnected();
    auto* aud = engine_.audio();
    s.audioAvailable = aud != nullptr;
    s.inputs.resize(size_t(engine_.inputCount()));
    for (int i = 0; i < engine_.inputCount(); ++i) {
        auto& in = s.inputs[size_t(i)];
        in.ref = engine_.inputRef(i);
        in.type = int(engine_.inputType(i));
        in.connected = engine_.inputStatus(i).connected;
        const auto m = engine_.inputMediaState(i);
        in.media = {m.available,     m.playing,       m.loop,
                    m.atEnd,         m.playlistIndex, m.playlistSize};
        if (aud && i < aud->inputCount()) {
            const auto& ch = aud->channel(i);
            in.audioMute = ch.mute.load(std::memory_order_relaxed);
            in.audioSolo = ch.solo.load(std::memory_order_relaxed);
            in.audioGain = ch.gain.load(std::memory_order_relaxed);
        }
    }
    return s;
}

void ControlServer::apply(const Request& r, Client& c) {
    using Op = Request::Op;
    const int nIn = engine_.inputCount();
    auto checkInput = [&](int idx) {
        if (idx >= 0 && idx < nIn) return true;
        sendError(c, "input " + std::to_string(idx + 1) + " out of range (1.." +
                         std::to_string(nIn) + ")");
        return false;
    };
    auto checkDsk = [&](int k) {
        if (k >= 0 && k < kDskCount) return true;
        sendError(c, "dsk " + std::to_string(k + 1) + " out of range (1.." +
                         std::to_string(kDskCount) + ")");
        return false;
    };

    switch (r.op) {
        case Op::Cut: engine_.post({Command::Type::Cut, 0, 0, 0.f}); break;
        case Op::Auto: engine_.post({Command::Type::Auto, 0, 0, 0.f}); break;
        case Op::Ftb:
            engine_.post({Command::Type::FadeToBlack, 0, 0, 0.f});
            break;
        case Op::SetProgram:
            if (checkInput(r.a))
                engine_.post({Command::Type::SetProgram, r.a, 0, 0.f});
            break;
        case Op::SetPreview:
            if (checkInput(r.a))
                engine_.post({Command::Type::SetPreview, r.a, 0, 0.f});
            break;
        case Op::SetTransition: {
            // 0 duration / negative softness = keep the current values.
            const auto ui = engine_.uiState();
            engine_.post({Command::Type::SetTransition, r.a,
                          r.b > 0 ? r.b : ui.transDur,
                          r.f >= 0.f ? r.f : ui.transSoftness});
            break;
        }
        case Op::TbarBegin:
            engine_.post({Command::Type::TbarBegin, 0, 0, 0.f});
            break;
        case Op::TbarSet:
            engine_.post({Command::Type::TbarSet, 0, 0, r.f});
            break;
        case Op::TbarEnd:
            engine_.post({Command::Type::TbarEnd, 0, 0, 0.f});
            break;
        case Op::DskSet: {
            if (!checkDsk(r.a)) break;
            // The engine only has toggle; reach the requested end state by
            // toggling conditionally on the mirrored target.
            const bool on = engine_.uiState().dskOn[r.a];
            if (r.b == 2 || (r.b == 1) != on)
                engine_.post({Command::Type::DskToggle, r.a, 0, 0.f});
            break;
        }
        case Op::SetDskSource:
            if (checkDsk(r.a) && checkInput(r.b))
                engine_.post({Command::Type::SetDskSource, r.a, r.b, 0.f});
            break;
        case Op::SetDskFade:
            if (checkDsk(r.a))
                engine_.post({Command::Type::SetDskFade, r.a, r.b, 0.f});
            break;
        case Op::MediaPlay:
        case Op::MediaPause:
            if (checkInput(r.a))
                engine_.post({Command::Type::MediaSetPlaying, r.a,
                              r.op == Op::MediaPlay ? 1 : 0, 0.f});
            break;
        case Op::MediaRestart:
            if (checkInput(r.a))
                engine_.post({Command::Type::MediaRestart, r.a, 0, 0.f});
            break;
        case Op::MediaStep:
            if (checkInput(r.a))
                engine_.post({Command::Type::MediaStep, r.a, r.b, 0.f});
            break;
        case Op::MediaLoop:
            if (checkInput(r.a))
                engine_.post({Command::Type::MediaSetLoop, r.a, r.b, 0.f});
            break;
        case Op::RecordStart:
        case Op::CleanRecordStart:
        case Op::RecordToggle:
        case Op::CleanRecordToggle: {
            const bool clean =
                r.op == Op::CleanRecordStart || r.op == Op::CleanRecordToggle;
            const auto state = clean ? engine_.cleanRecordingState()
                                     : engine_.recordingState();
            const bool running = state.active || state.pending;
            const bool toggle =
                r.op == Op::RecordToggle || r.op == Op::CleanRecordToggle;
            if (running) {
                if (!toggle) {
                    sendError(c, clean ? "clean record already running"
                                       : "record already running");
                    break;
                }
                clean ? engine_.requestCleanRecording({})
                      : engine_.requestRecording({});
                break;
            }
            const std::string path =
                r.s.empty() ? defaultRecordPath(clean) : r.s;
            clean ? engine_.requestCleanRecording(path)
                  : engine_.requestRecording(path);
            break;
        }
        case Op::RecordStop: engine_.requestRecording({}); break;
        case Op::CleanRecordStop: engine_.requestCleanRecording({}); break;
        case Op::AudioMute:
        case Op::AudioSolo:
        case Op::AudioGain: {
            auto* aud = engine_.audio();
            if (!aud) {
                sendError(c, "audio is disabled");
                break;
            }
            if (!checkInput(r.a) || r.a >= aud->inputCount()) break;
            auto& ch = aud->channel(r.a);
            if (r.op == Op::AudioGain) {
                ch.gain.store(r.f);
            } else {
                auto& flag = r.op == Op::AudioMute ? ch.mute : ch.solo;
                flag.store(r.b == 2 ? !flag.load() : r.b == 1);
            }
            break;
        }
        case Op::Subscribe:
            c.subscribed = true;
            send(c, lastState_.empty() ? toJson(snapshot()) : lastState_);
            break;
        case Op::Unsubscribe: c.subscribed = false; break;
        case Op::GetState: send(c, toJson(snapshot())); break;
        case Op::Ping: send(c, "{\"event\":\"pong\"}"); break;
    }
}

void ControlServer::run(std::stop_token st) {
    while (!st.stop_requested()) {
        std::vector<pollfd> fds;
        fds.push_back({listenFd_, POLLIN, 0});
        fds.push_back({wakeFd_, POLLIN, 0});
        for (const auto& c : clients_)
            fds.push_back({c.fd,
                           short(POLLIN | (c.out.empty() ? 0 : POLLOUT)), 0});
        if (poll(fds.data(), nfds_t(fds.size()), kPollMs) < 0 &&
            errno != EINTR) {
            MOO_LOGE("control: poll: %s", strerror(errno));
            return;
        }
        if (st.stop_requested()) return;

        // Clients accepted below are not in this iteration's fds; the read
        // phase must only walk the clients that were polled.
        const size_t nPolled = clients_.size();
        if (fds[0].revents & POLLIN) {
            const int fd = accept4(listenFd_, nullptr, nullptr,
                                   SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd >= 0) {
                if (clients_.size() >= kMaxClients) {
                    close(fd);
                } else {
                    const int one = 1;
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    Client c;
                    c.fd = fd;
                    send(c, "{\"event\":\"hello\",\"name\":\"MooSwitcher\","
                            "\"protocol\":1}");
                    clients_.push_back(std::move(c));
                }
            }
        }

        // Reads + request handling. Client fds start at fds[2].
        for (size_t i = 0; i < nPolled; ++i) {
            Client& c = clients_[i];
            if (c.in == "\x01") {  // poisoned by send() overflow
                close(c.fd);
                c.fd = -1;
                continue;
            }
            if (!(fds[i + 2].revents & (POLLIN | POLLERR | POLLHUP))) continue;
            char buf[4096];
            for (;;) {
                const ssize_t n = recv(c.fd, buf, sizeof buf, 0);
                if (n > 0) {
                    c.in.append(buf, size_t(n));
                    if (c.in.size() > kMaxLine * 4) {  // garbage flood
                        close(c.fd);
                        c.fd = -1;
                        break;
                    }
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                close(c.fd);  // orderly close or hard error
                c.fd = -1;
                break;
            }
            if (c.fd < 0) continue;
            size_t nl;
            while ((nl = c.in.find('\n')) != std::string::npos) {
                std::string line = c.in.substr(0, nl);
                c.in.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.size() > kMaxLine) continue;
                std::string err;
                if (const auto req = parseLine(line, err))
                    apply(*req, c);
                else if (!err.empty())
                    sendError(c, err);
            }
        }

        // State publish on change.
        const std::string state = toJson(snapshot());
        const bool changed = state != lastState_;
        if (changed) lastState_ = state;
        for (auto& c : clients_) {
            if (c.fd < 0) continue;
            if (changed && c.subscribed) send(c, lastState_);
            if (c.out.empty()) continue;
            const ssize_t n =
                ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
            if (n > 0)
                c.out.erase(0, size_t(n));
            else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                close(c.fd);
                c.fd = -1;
            }
        }
        std::erase_if(clients_, [](const Client& c) { return c.fd < 0; });
    }
}

}  // namespace moo::ctl
