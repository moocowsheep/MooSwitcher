/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <catch2/catch_test_macros.hpp>

#include "media/Playlist.h"
#include "media/StillImage.h"

using namespace moo::media;

TEST_CASE("playlist auto-advance stops or wraps at the final clip") {
    REQUIRE(playlistAdvance(0, 3, false) == 1);
    REQUIRE(playlistAdvance(1, 3, false) == 2);
    CHECK_FALSE(playlistAdvance(2, 3, false).has_value());
    REQUIRE(playlistAdvance(2, 3, true) == 0);
    CHECK_FALSE(playlistAdvance(0, 0, true).has_value());
}

TEST_CASE("playlist operator steps wrap in both directions") {
    CHECK(playlistStep(0, 3, 1) == 1);
    CHECK(playlistStep(2, 3, 1) == 0);
    CHECK(playlistStep(2, 3, -1) == 1);
    CHECK(playlistStep(0, 3, -1) == 2);
    CHECK(playlistStep(0, 0, 1) == 0);
}

TEST_CASE("playlist trim points are inclusive at in and exclusive at out") {
    PlaylistItem item{"clip.mkv", 500, 1'500};
    CHECK(playlistBeforeIn(item, 499));
    CHECK_FALSE(playlistBeforeIn(item, 500));
    CHECK_FALSE(playlistAtOrPastOut(item, 1'499));
    CHECK(playlistAtOrPastOut(item, 1'500));

    item = {"clip.mkv", -50, 20};
    normalizePlaylistItem(item);
    CHECK(item.inMs == 0);
    CHECK(item.outMs == 20);

    item = {"clip.mkv", 500, 500};
    normalizePlaylistItem(item);
    CHECK(item.inMs == 500);
    CHECK(item.outMs == 0);

    item = {"clip.mkv", 0, 0, 200};
    normalizePlaylistItem(item);
    CHECK(item.speedPermille == 1000);

    item = {"clip.mkv", 0, 0, 4000};
    normalizePlaylistItem(item);
    CHECK(item.speedPermille == 4000);
}

TEST_CASE("playlist speed scales source time onto wall time") {
    PlaylistItem item{"clip.mkv", 0, 0, 250};
    CHECK(playlistWallDurationNs(item, 1'000'000'000) == 4'000'000'000);
    item.speedPermille = 500;
    CHECK(playlistWallDurationNs(item, 1'000'000'000) == 2'000'000'000);
    item.speedPermille = 1000;
    CHECK(playlistWallDurationNs(item, 1'000'000'000) == 1'000'000'000);
    item.speedPermille = 2000;
    CHECK(playlistWallDurationNs(item, 1'000'000'000) == 500'000'000);
    item.speedPermille = 4000;
    CHECK(playlistWallDurationNs(item, 1'000'000'000) == 250'000'000);
}

TEST_CASE("still-image path inference recognizes supported raster formats") {
    CHECK(isStillImagePath("/show/Logo.PNG"));
    CHECK(isStillImagePath("lower.third.webp"));
    CHECK(isStillImagePath("C:\\show\\matte.TIFF"));
    CHECK(isStillImagePath("plate.exr"));
    CHECK_FALSE(isStillImagePath("/show/roll-in.mkv"));
    CHECK_FALSE(isStillImagePath("/show/image"));
    CHECK_FALSE(isStillImagePath("/show.with.dot/image"));
}
