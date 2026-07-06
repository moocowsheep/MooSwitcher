#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/SwitcherCore.h"

using Catch::Approx;
using moo::CompositeJob;
using moo::SwitcherCore;
using moo::TransitionType;

TEST_CASE("initial state: program 0 on air, no transition") {
    SwitcherCore sw;
    const CompositeJob j = sw.tick(0);
    REQUIRE(j.programSrc == 0);
    REQUIRE(j.previewSrc == 1);
    REQUIRE(j.alpha == 0.f);
    REQUIRE_FALSE(j.transitionActive);
    REQUIRE(j.ftb == 0.f);
}

TEST_CASE("cut swaps program and preview instantly") {
    SwitcherCore sw;
    sw.cut();
    const CompositeJob j = sw.tick(0);
    REQUIRE(j.programSrc == 1);
    REQUIRE(j.previewSrc == 0);
    REQUIRE(j.alpha == 0.f);
}

TEST_CASE("auto transition ramps by tick time and lands with a swap") {
    SwitcherCore sw;
    sw.setTransition(TransitionType::Mix, 10, 0.f);
    sw.autoTransition(100);

    REQUIRE(sw.tick(100).alpha == Approx(0.1f));
    REQUIRE(sw.tick(104).alpha == Approx(0.5f));
    REQUIRE(sw.tick(108).alpha == Approx(0.9f));

    const CompositeJob landed = sw.tick(109);  // (109-100+1)/10 = 1.0
    REQUIRE(landed.alpha == 0.f);
    REQUIRE(landed.programSrc == 1);
    REQUIRE(landed.previewSrc == 0);
    REQUIRE_FALSE(landed.transitionActive);
}

TEST_CASE("auto transition is time-based across skipped ticks") {
    SwitcherCore sw;
    sw.setTransition(TransitionType::Mix, 10, 0.f);
    sw.autoTransition(0);
    REQUIRE(sw.tick(0).alpha == Approx(0.1f));
    // Ticks 1..8 never rendered (overload); alpha reflects elapsed time.
    const CompositeJob j = sw.tick(9);
    REQUIRE(j.alpha == 0.f);  // (9-0+1)/10 = 1.0 -> landed
    REQUIRE(j.programSrc == 1);
}

TEST_CASE("T-bar is manual alpha; releasing mid-travel holds") {
    SwitcherCore sw;
    sw.tbarBegin();
    sw.tbarSet(0.4f);
    REQUIRE(sw.tick(0).alpha == Approx(0.4f));
    sw.tbarEnd();
    REQUIRE(sw.tick(1).alpha == Approx(0.4f));  // held
    REQUIRE(sw.tick(2).transitionActive);
}

TEST_CASE("T-bar completes at full travel and re-arms") {
    SwitcherCore sw;
    sw.tbarBegin();
    sw.tbarSet(0.9f);
    sw.tbarSet(1.0f);
    const CompositeJob j = sw.tick(0);
    REQUIRE(j.programSrc == 1);
    REQUIRE(j.previewSrc == 0);
    REQUIRE(j.alpha == 0.f);
    REQUIRE_FALSE(j.transitionActive);
}

TEST_CASE("cut during auto transition completes it") {
    SwitcherCore sw;
    sw.setTransition(TransitionType::Mix, 30, 0.f);
    sw.autoTransition(0);
    sw.tick(5);
    sw.cut();
    const CompositeJob j = sw.tick(6);
    REQUIRE(j.programSrc == 1);
    REQUIRE(j.alpha == 0.f);
    REQUIRE_FALSE(j.transitionActive);
}

TEST_CASE("bus assign cancels an active transition without swapping") {
    SwitcherCore sw;
    sw.autoTransition(0);
    sw.tick(2);
    sw.setPreview(3);
    const CompositeJob j = sw.tick(3);
    REQUIRE(j.programSrc == 0);  // unchanged: no swap happened
    REQUIRE(j.previewSrc == 3);
    REQUIRE(j.alpha == 0.f);
    REQUIRE_FALSE(j.transitionActive);
}

TEST_CASE("FTB ramps to black and back, rate-correct across skips") {
    SwitcherCore sw;
    sw.setFtbDuration(10);
    sw.tick(0);  // establish tick baseline
    sw.fadeToBlack();
    REQUIRE(sw.tick(1).ftb == Approx(0.1f));
    REQUIRE(sw.tick(6).ftb == Approx(0.6f));   // 5 skipped ticks advance 0.5
    REQUIRE(sw.tick(20).ftb == Approx(1.0f));  // clamped at full black
    REQUIRE(sw.ftbEngaged());

    sw.fadeToBlack();  // toggle off
    REQUIRE(sw.tick(25).ftb == Approx(0.5f));
    REQUIRE(sw.tick(40).ftb == Approx(0.0f));
    REQUIRE_FALSE(sw.ftbEngaged());
}

TEST_CASE("wipe parameters pass through to the job") {
    SwitcherCore sw;
    sw.setTransition(TransitionType::WipeLR, 20, 0.25f);
    sw.autoTransition(0);
    const CompositeJob j = sw.tick(0);
    REQUIRE(j.trans == TransitionType::WipeLR);
    REQUIRE(j.softness == Approx(0.25f));
}
