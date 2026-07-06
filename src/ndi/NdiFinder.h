#pragma once
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ndi/NdiLib.h"

namespace moo {

// NDI discovery thread; keeps a name/url snapshot for substring lookup.
class NdiFinder {
public:
    struct Source {
        std::string name;
        std::string url;
    };

    NdiFinder();
    ~NdiFinder();

    std::optional<Source> lookup(const std::string& lowerSubstr) const;
    std::vector<Source> snapshot() const;

private:
    void run(std::stop_token st);

    NDIlib_find_instance_t finder_ = nullptr;
    mutable std::mutex m_;
    std::vector<Source> sources_;
    std::jthread thread_;
};

}  // namespace moo
