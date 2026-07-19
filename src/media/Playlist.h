#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace moo::media {

struct PlaylistItem {
    std::string path;
    int64_t inMs = 0;   // inclusive, relative to the source timeline
    int64_t outMs = 0;  // exclusive; 0 = play through end of file
    int speedPermille = 1000;  // 250..4000 = 0.25x..4.00x

    PlaylistItem() = default;
    PlaylistItem(std::string itemPath, int64_t itemInMs = 0,
                 int64_t itemOutMs = 0, int itemSpeedPermille = 1000)
        : path(std::move(itemPath)), inMs(itemInMs), outMs(itemOutMs),
          speedPermille(itemSpeedPermille) {}

    bool operator==(const PlaylistItem&) const = default;
};

inline void normalizePlaylistItem(PlaylistItem& item) {
    if (item.inMs < 0) item.inMs = 0;
    if (item.outMs < 0 || (item.outMs > 0 && item.outMs <= item.inMs))
        item.outMs = 0;
    if (item.speedPermille < 250 || item.speedPermille > 4000)
        item.speedPermille = 1000;
}

inline bool playlistBeforeIn(const PlaylistItem& item, int64_t positionMs) {
    return positionMs < item.inMs;
}

inline bool playlistAtOrPastOut(const PlaylistItem& item,
                                int64_t positionMs) {
    return item.outMs > 0 && positionMs >= item.outMs;
}

inline int64_t playlistWallDurationNs(const PlaylistItem& item,
                                      int64_t sourceDurationNs) {
    return sourceDurationNs * 1000 /
           (item.speedPermille > 0 ? item.speedPermille : 1000);
}

// Automatic playback advances until the end of the list. Looping wraps the
// whole list; with loop off, the final item remains selected and playback
// stops at its end.
inline std::optional<size_t> playlistAdvance(size_t current, size_t count,
                                             bool loop) {
    if (count == 0) return std::nullopt;
    if (current + 1 < count) return current + 1;
    if (loop) return size_t{0};
    return std::nullopt;
}

// Operator previous/next controls always wrap, independent of automatic-loop
// mode. That keeps every item reachable after a non-looping list has ended.
inline size_t playlistStep(size_t current, size_t count, int direction) {
    if (count == 0) return 0;
    current %= count;
    if (direction < 0) return current == 0 ? count - 1 : current - 1;
    return current + 1 == count ? 0 : current + 1;
}

}  // namespace moo::media
