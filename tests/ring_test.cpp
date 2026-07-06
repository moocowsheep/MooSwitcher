#include <catch2/catch_test_macros.hpp>

#include <thread>
#include <vector>

#include "core/Spsc.h"

using moo::LatestMailbox;
using moo::SpscRing;

TEST_CASE("SpscRing basic push/pop and full/empty") {
    SpscRing<int> r(4);
    REQUIRE(r.capacity() == 4);

    int v = 0;
    REQUIRE_FALSE(r.pop(v));
    for (int i = 0; i < 4; ++i) REQUIRE(r.push(i));
    REQUIRE_FALSE(r.push(99));  // full
    for (int i = 0; i < 4; ++i) {
        REQUIRE(r.pop(v));
        REQUIRE(v == i);
    }
    REQUIRE_FALSE(r.pop(v));
}

TEST_CASE("SpscRing cross-thread ordering") {
    SpscRing<int> r(1024);
    constexpr int N = 200000;
    std::thread producer([&] {
        for (int i = 0; i < N;) {
            if (r.push(i)) ++i;
        }
    });
    int expect = 0, v = 0;
    while (expect < N) {
        if (r.pop(v)) {
            REQUIRE(v == expect);
            ++expect;
        }
    }
    producer.join();
}

TEST_CASE("LatestMailbox keeps newest and reports seq") {
    LatestMailbox<int> mb;
    REQUIRE_FALSE(mb.takeNewer(0).has_value());  // never published

    mb.publish(10);
    mb.publish(20);
    mb.publish(30);

    auto item = mb.takeNewer(0);
    REQUIRE(item.has_value());
    REQUIRE(item->value == 30);
    REQUIRE(item->seq == 3);  // two overwrites visible as seq gap

    REQUIRE_FALSE(mb.takeNewer(item->seq).has_value());  // unchanged
    mb.publish(40);
    auto next = mb.takeNewer(item->seq);
    REQUIRE(next.has_value());
    REQUIRE(next->value == 40);
    REQUIRE(next->seq == 4);
}

TEST_CASE("LatestMailbox retains recent publishes as fallback candidates") {
    LatestMailbox<int> mb;
    LatestMailbox<int>::Item c[LatestMailbox<int>::kKeep];
    REQUIRE(mb.takeNewerCandidates(0, c) == 0);

    mb.publish(10);
    REQUIRE(mb.takeNewerCandidates(0, c) == 1);
    REQUIRE(c[0].value == 10);
    REQUIRE(c[0].seq == 1);

    mb.publish(20);
    mb.publish(30);
    mb.publish(40);
    REQUIRE(mb.takeNewerCandidates(0, c) == 3);  // capped at kKeep
    REQUIRE(c[0].value == 40);
    REQUIRE(c[0].seq == 4);
    REQUIRE(c[1].value == 30);
    REQUIRE(c[2].value == 20);

    // Already has seq 3: only the newest is newer.
    REQUIRE(mb.takeNewerCandidates(3, c) == 1);
    REQUIRE(c[0].value == 40);

    REQUIRE(mb.takeNewerCandidates(4, c) == 0);  // fully caught up
}
