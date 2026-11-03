#pragma once
// Resource profiles for the AI worker: how much of the PC AceStudio may use.
//
//   Eco          low power: fewer CPU threads, below-normal priority, the GPU
//                rests between diffusion steps, models are released soon
//                after use, a cancel stops even a model load at once.
//   Balanced     the default: below-normal priority so Windows and drawing
//                stay responsive, half the CPU threads, models kept for ten
//                minutes of idle time.
//   Performance  fastest: all threads, normal priority, models kept loaded,
//                the background-removal model kept on the GPU when it fits.
//
// Any single limit can be overridden (Settings > AI > Advanced).

#include <optional>
#include <string>
#include <string_view>

namespace acestudio::ai {

enum class Profile { Eco, Balanced, Performance };
const char* toString(Profile p) noexcept;
std::optional<Profile> profileFromString(std::string_view s) noexcept;

struct ResourceLimits {
    int idleUnloadMinutes = 10;      // stop the worker after this long idle; 0 = never
    int cpuThreads = 0;              // 0 = all cores
    bool lowPriority = true;         // below-normal process priority
    bool keepCutoutLoaded = false;   // keep the background-removal session on the GPU between jobs
    int gpuDutyPercent = 100;        // < 100: the worker rests between steps so the GPU averages this share
    bool pauseWhileDrawing = true;   // hold the next queued AI job while the user is drawing
    int cancelGraceSeconds = 10;     // a cancelled job is force-stopped after this
    bool cancelDuringLoad = false;   // a cancel also interrupts a model load (restarts the worker)
};

// Overrides: -1 = keep the profile's value.
struct LimitOverrides {
    int idleUnloadMinutes = -1, cpuThreads = -1, lowPriority = -1, keepCutoutLoaded = -1, gpuDutyPercent = -1,
        pauseWhileDrawing = -1;
};

// `cores` = logical CPUs, `vramMb` = the GPU's total memory (0 = no GPU).
ResourceLimits resolveLimits(Profile p, const LimitOverrides& o, int cores, int vramMb);

} // namespace acestudio::ai
