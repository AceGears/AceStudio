#include "acestudio/core/ResourceProfile.h"

#include <catch2/catch_test_macros.hpp>

using namespace acestudio::ai;

TEST_CASE("resource profiles trade speed for a lighter load", "[ai][profile]") {
    const LimitOverrides none;
    const auto eco = resolveLimits(Profile::Eco, none, 16, 8192);
    const auto bal = resolveLimits(Profile::Balanced, none, 16, 8192);
    const auto perf = resolveLimits(Profile::Performance, none, 16, 8192);
    CHECK(eco.cpuThreads < bal.cpuThreads);
    CHECK(perf.cpuThreads == 0);                  // all cores
    CHECK(eco.gpuDutyPercent < 100);
    CHECK(bal.gpuDutyPercent == 100);
    CHECK(eco.lowPriority);
    CHECK(bal.lowPriority);
    CHECK_FALSE(perf.lowPriority);
    CHECK(eco.idleUnloadMinutes < bal.idleUnloadMinutes);
    CHECK(perf.idleUnloadMinutes == 0);           // never released
    CHECK(eco.cancelDuringLoad);
    // The cutout session stays on the GPU only with room for it (measured on 8 GB).
    CHECK_FALSE(bal.keepCutoutLoaded);
    CHECK(resolveLimits(Profile::Balanced, none, 16, 12288).keepCutoutLoaded);
    CHECK_FALSE(perf.keepCutoutLoaded);
    CHECK(resolveLimits(Profile::Performance, none, 16, 12288).keepCutoutLoaded);
}

TEST_CASE("single limits override the profile", "[ai][profile]") {
    LimitOverrides o;
    o.cpuThreads = 3;
    o.gpuDutyPercent = 5;     // clamped to a sane minimum
    o.idleUnloadMinutes = 0;
    const auto l = resolveLimits(Profile::Eco, o, 16, 8192);
    CHECK(l.cpuThreads == 3);
    CHECK(l.gpuDutyPercent == 20);
    CHECK(l.idleUnloadMinutes == 0);
    CHECK(profileFromString("performance") == Profile::Performance);
    CHECK_FALSE(profileFromString("turbo"));
}
