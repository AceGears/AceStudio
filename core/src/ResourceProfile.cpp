#include "acestudio/core/ResourceProfile.h"

#include <algorithm>

namespace acestudio::ai {

const char* toString(Profile p) noexcept {
    switch (p) {
    case Profile::Eco: return "eco";
    case Profile::Balanced: return "balanced";
    case Profile::Performance: return "performance";
    }
    return "balanced";
}

std::optional<Profile> profileFromString(std::string_view s) noexcept {
    for (Profile p : {Profile::Eco, Profile::Balanced, Profile::Performance})
        if (s == toString(p)) return p;
    return std::nullopt;
}

ResourceLimits resolveLimits(Profile p, const LimitOverrides& o, int cores, int vramMb) {
    cores = std::max(1, cores);
    ResourceLimits l;
    switch (p) {
    case Profile::Eco:
        l.idleUnloadMinutes = 2;
        l.cpuThreads = std::max(2, cores / 4);
        l.lowPriority = true;
        l.keepCutoutLoaded = false;
        l.gpuDutyPercent = 60;
        l.pauseWhileDrawing = true;
        l.cancelGraceSeconds = 5;
        l.cancelDuringLoad = true;
        break;
    case Profile::Balanced:
        l.idleUnloadMinutes = 10;
        l.cpuThreads = std::max(2, cores / 2);
        l.lowPriority = true;
        // Measured on an 8 GB laptop GPU: the background-removal session kept
        // next to FLUX.2 klein pushed klein into shared memory (steps 25x
        // slower). Only cards with room for both keep it.
        l.keepCutoutLoaded = vramMb >= 12 * 1024;
        l.gpuDutyPercent = 100;
        l.pauseWhileDrawing = true;
        l.cancelGraceSeconds = 10;
        break;
    case Profile::Performance:
        l.idleUnloadMinutes = 0;
        l.cpuThreads = 0;
        l.lowPriority = false;
        l.keepCutoutLoaded = vramMb >= 10 * 1024;
        l.gpuDutyPercent = 100;
        l.pauseWhileDrawing = false;
        l.cancelGraceSeconds = 10;
        break;
    }
    if (o.idleUnloadMinutes >= 0) l.idleUnloadMinutes = std::min(o.idleUnloadMinutes, 24 * 60);
    if (o.cpuThreads >= 0) l.cpuThreads = std::min(o.cpuThreads, cores);
    if (o.lowPriority >= 0) l.lowPriority = o.lowPriority != 0;
    if (o.keepCutoutLoaded >= 0) l.keepCutoutLoaded = o.keepCutoutLoaded != 0;
    if (o.gpuDutyPercent >= 0) l.gpuDutyPercent = std::clamp(o.gpuDutyPercent, 20, 100);
    if (o.pauseWhileDrawing >= 0) l.pauseWhileDrawing = o.pauseWhileDrawing != 0;
    return l;
}

} // namespace acestudio::ai
