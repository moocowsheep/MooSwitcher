#include "ndi/NdiFinder.h"

#include <algorithm>
#include <cctype>

#include "core/Log.h"

namespace moo {

namespace {
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}
}  // namespace

NdiFinder::NdiFinder() {
    NDIlib_find_create_t desc{};
    desc.show_local_sources = true;
    finder_ = NDIlib_find_create_v2(&desc);
    if (!finder_) {
        MOO_LOGE("NDIlib_find_create_v2 failed");
        return;
    }
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
}

NdiFinder::~NdiFinder() {
    thread_ = {};  // request stop + join
    if (finder_) NDIlib_find_destroy(finder_);
}

void NdiFinder::run(std::stop_token st) {
    while (!st.stop_requested()) {
        NDIlib_find_wait_for_sources(finder_, 500);
        uint32_t count = 0;
        const NDIlib_source_t* list = NDIlib_find_get_current_sources(finder_, &count);
        std::vector<Source> next;
        next.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
            next.push_back({list[i].p_ndi_name ? list[i].p_ndi_name : "",
                            list[i].p_url_address ? list[i].p_url_address : ""});
        std::lock_guard lk(m_);
        sources_ = std::move(next);
    }
}

std::optional<NdiFinder::Source> NdiFinder::lookup(const std::string& lowerSubstr) const {
    std::lock_guard lk(m_);
    for (const auto& s : sources_)
        if (lower(s.name).find(lowerSubstr) != std::string::npos) return s;
    return std::nullopt;
}

std::vector<NdiFinder::Source> NdiFinder::snapshot() const {
    std::lock_guard lk(m_);
    return sources_;
}

}  // namespace moo
