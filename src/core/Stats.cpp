#include "core/Stats.h"

#include <map>
#include <memory>
#include <mutex>

namespace moo {

namespace {
struct Registry {
    std::mutex m;
    std::map<std::string, std::unique_ptr<Stats::Counter>> counters;
};
Registry& registry() {
    static Registry r;
    return r;
}
}  // namespace

Stats::Counter& Stats::counter(const std::string& name) {
    auto& r = registry();
    std::lock_guard lk(r.m);
    auto& slot = r.counters[name];
    if (!slot) slot = std::make_unique<Counter>();
    return *slot;
}

std::vector<Stats::Sample> Stats::snapshot() {
    auto& r = registry();
    std::lock_guard lk(r.m);
    std::vector<Sample> out;
    out.reserve(r.counters.size());
    for (auto& [name, c] : r.counters) out.push_back({name, c->value()});
    return out;
}

}  // namespace moo
