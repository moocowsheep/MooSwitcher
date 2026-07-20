/* Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

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

TEST_CASE("DSK ramps up over its duration and clamps, rate-correct") {
    SwitcherCore sw;
    sw.setDskSource(0, 2);
    sw.setDskDuration(0, 10);
    sw.tick(0);  // establish tick baseline
    sw.dskToggle(0);
    REQUIRE(sw.dskOn(0));
    REQUIRE(sw.tick(1).dskLevel[0] == Approx(0.1f));
    REQUIRE(sw.tick(6).dskLevel[0] == Approx(0.6f));  // skipped ticks add 0.5
    const CompositeJob j = sw.tick(20);
    REQUIRE(j.dskLevel[0] == Approx(1.0f));  // clamped
    REQUIRE(j.dskSrc[0] == 2);
    REQUIRE(j.dskOn[0]);
    REQUIRE_FALSE(j.dskOn[1]);
    REQUIRE(j.dskLevel[1] == 0.f);
}

TEST_CASE("DSK toggle mid-fade reverses from the current level") {
    SwitcherCore sw;
    sw.setDskDuration(0, 10);
    sw.tick(0);
    sw.dskToggle(0);
    REQUIRE(sw.tick(5).dskLevel[0] == Approx(0.5f));
    sw.dskToggle(0);  // reverse mid-fade
    REQUIRE_FALSE(sw.dskOn(0));
    REQUIRE(sw.tick(7).dskLevel[0] == Approx(0.3f));
    REQUIRE(sw.tick(15).dskLevel[0] == Approx(0.0f));
}

TEST_CASE("DSK is independent of cut/auto/FTB; source change keeps level") {
    SwitcherCore sw;
    sw.setDskDuration(0, 10);
    sw.tick(0);
    sw.dskToggle(0);
    sw.tick(10);
    REQUIRE(sw.dskLevel(0) == Approx(1.0f));

    sw.cut();
    REQUIRE(sw.tick(11).dskLevel[0] == Approx(1.0f));
    sw.autoTransition(12);
    sw.tick(13);
    REQUIRE(sw.dskLevel(0) == Approx(1.0f));
    sw.fadeToBlack();
    const CompositeJob j = sw.tick(20);
    REQUIRE(j.dskLevel[0] == Approx(1.0f));  // FTB rides over, level unmoved
    REQUIRE(j.ftb > 0.f);

    sw.setDskSource(0, 3);  // live source change: level untouched
    const CompositeJob j2 = sw.tick(21);
    REQUIRE(j2.dskSrc[0] == 3);
    REQUIRE(j2.dskLevel[0] == Approx(1.0f));
}

TEST_CASE("two DSKs ramp independently with different durations") {
    SwitcherCore sw;
    sw.setDskDuration(0, 10);
    sw.setDskDuration(1, 20);
    sw.tick(0);
    sw.dskToggle(0);
    sw.dskToggle(1);
    const CompositeJob j = sw.tick(5);
    REQUIRE(j.dskLevel[0] == Approx(0.5f));
    REQUIRE(j.dskLevel[1] == Approx(0.25f));
    REQUIRE(sw.tick(10).dskLevel[0] == Approx(1.0f));
    REQUIRE(sw.tick(20).dskLevel[1] == Approx(1.0f));
}

TEST_CASE("DSK keyer index bounds are enforced") {
    SwitcherCore sw;
    sw.dskToggle(-1);
    sw.dskToggle(2);
    sw.setDskSource(5, 1);
    sw.setDskDuration(-3, 5);
    sw.setDskTie(2, true);
    sw.setDskAudioFollow(-1, true);
    const CompositeJob j = sw.tick(0);
    REQUIRE(j.dskLevel[0] == 0.f);
    REQUIRE(j.dskLevel[1] == 0.f);
    REQUIRE_FALSE(j.dskOn[0]);
    REQUIRE_FALSE(j.dskTie[0]);
}

TEST_CASE("tied DSK rides an auto transition in, ignoring its own duration") {
    SwitcherCore sw;
    sw.setTransition(TransitionType::Mix, 10, 0.f);
    sw.setDskDuration(0, 100);  // must not matter while tied
    sw.setDskTie(0, true);
    sw.tick(0);
    REQUIRE(sw.dskWillBeOn(0));  // off + tied: next transition brings it in
    sw.autoTransition(100);
    REQUIRE(sw.dskOn(0));  // destination committed at arm time
    REQUIRE(sw.tick(100).dskLevel[0] == Approx(0.1f));
    REQUIRE(sw.tick(104).dskLevel[0] == Approx(0.5f));
    const CompositeJob landed = sw.tick(109);
    REQUIRE(landed.programSrc == 1);
    REQUIRE(landed.dskLevel[0] == Approx(1.0f));
    REQUIRE(landed.dskOn[0]);
    // Engaged + still tied: the next transition would take it out.
    REQUIRE_FALSE(sw.dskWillBeOn(0));
}

TEST_CASE("tied DSK fades out with the transition from an engaged state") {
    SwitcherCore sw;
    sw.setTransition(TransitionType::Mix, 10, 0.f);
    sw.setDskDuration(0, 1);
    sw.tick(0);
    sw.dskToggle(0);
    sw.tick(1);
    REQUIRE(sw.dskLevel(0) == Approx(1.0f));
    sw.setDskTie(0, true);
    sw.autoTransition(10);
    REQUIRE(sw.tick(14).dskLevel[0] == Approx(0.5f));
    const CompositeJob landed = sw.tick(19);
    REQUIRE(landed.dskLevel[0] == Approx(0.0f));
    REQUIRE_FALSE(landed.dskOn[0]);
}

TEST_CASE("tied DSK follows the T-bar and lands at full travel") {
    SwitcherCore sw;
    sw.setDskTie(0, true);
    sw.tbarBegin();
    sw.tbarSet(0.25f);
    REQUIRE(sw.tick(0).dskLevel[0] == Approx(0.25f));
    sw.tbarEnd();  // held mid-travel: keyer holds too
    REQUIRE(sw.tick(1).dskLevel[0] == Approx(0.25f));
    sw.tbarBegin();  // re-grab continues, must not re-arm
    sw.tbarSet(1.0f);
    const CompositeJob j = sw.tick(2);
    REQUIRE(j.programSrc == 1);
    REQUIRE(j.dskLevel[0] == Approx(1.0f));
    REQUIRE(j.dskOn[0]);
}

TEST_CASE("cut executes the tied keyer toggle instantly") {
    SwitcherCore sw;
    sw.setDskTie(0, true);
    sw.tick(0);
    sw.cut();
    const CompositeJob j = sw.tick(1);
    REQUIRE(j.programSrc == 1);
    REQUIRE(j.dskLevel[0] == Approx(1.0f));
    REQUIRE(j.dskOn[0]);
    sw.cut();  // and back out
    REQUIRE(sw.tick(2).dskLevel[0] == Approx(0.0f));
}

TEST_CASE("cut mid-transition lands tied keyers with the buses") {
    SwitcherCore sw;
    sw.setTransition(TransitionType::Mix, 10, 0.f);
    sw.setDskTie(0, true);
    sw.tick(0);
    sw.autoTransition(1);
    sw.tick(5);
    sw.cut();
    const CompositeJob j = sw.tick(6);
    REQUIRE(j.programSrc == 1);
    REQUIRE(j.dskLevel[0] == Approx(1.0f));
}

TEST_CASE("bus assign cancels a tied transition; keyer ramps back") {
    SwitcherCore sw;
    sw.setTransition(TransitionType::Mix, 10, 0.f);
    sw.setDskDuration(0, 5);
    sw.setDskTie(0, true);
    sw.tick(0);
    sw.autoTransition(1);
    REQUIRE(sw.tick(5).dskLevel[0] == Approx(0.5f));
    sw.setPreview(3);  // cancel without swap
    REQUIRE_FALSE(sw.dskOn(0));  // destination reverted to off
    REQUIRE(sw.tick(6).dskLevel[0] == Approx(0.3f));  // own 5-tick rate
    REQUIRE(sw.tick(10).dskLevel[0] == Approx(0.0f));
}

TEST_CASE("untied DSK keeps its own fade during a transition") {
    SwitcherCore sw;
    sw.setTransition(TransitionType::Mix, 10, 0.f);
    sw.setDskDuration(0, 20);
    sw.tick(0);
    sw.dskToggle(0);
    sw.autoTransition(1);
    REQUIRE(sw.tick(5).dskLevel[0] == Approx(0.25f));  // 5/20, not 5/10
    const CompositeJob landed = sw.tick(10);
    REQUIRE(landed.programSrc == 1);
    REQUIRE(landed.dskLevel[0] == Approx(0.5f));  // still on its own clock
}

TEST_CASE("audio-follow and tie flags pass through to the job") {
    SwitcherCore sw;
    sw.setDskAudioFollow(1, true);
    sw.setDskTie(0, true);
    const CompositeJob j = sw.tick(0);
    REQUIRE(j.dskAudioFollow[1]);
    REQUIRE_FALSE(j.dskAudioFollow[0]);
    REQUIRE(j.dskTie[0]);
    REQUIRE(j.dskFutureOn[0]);        // off + tied -> comes in next
    REQUIRE_FALSE(j.dskFutureOn[1]);  // off + untied -> stays off
}
