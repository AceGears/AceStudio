#pragma once

// Editable body skeletons for pose-conditioned generation.
//
// Keypoints follow the 18-point layout that OpenPose ControlNets are trained
// on (nose, neck, shoulders/elbows/wrists, hips/knees/ankles, eyes, ears), and
// renderPose() draws the conditioning image in that convention: coloured limbs
// and joints on black. Coordinates are normalised to the image (0..1, y down)
// so one pose renders at any size. Presets generate whole cycles procedurally
// (side view), for animation generation.

#include "acestudio/core/Image.h"
#include "acestudio/core/Result.h"

#include <array>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace acestudio {

enum class Joint {
    Nose, Neck,
    RShoulder, RElbow, RWrist,
    LShoulder, LElbow, LWrist,
    RHip, RKnee, RAnkle,
    LHip, LKnee, LAnkle,
    REye, LEye, REar, LEar,
};
inline constexpr int kJointCount = 18;
std::string_view toString(Joint j) noexcept;

struct Keypoint {
    double x = 0, y = 0;     // normalised image coordinates
    bool visible = true;
    bool operator==(const Keypoint&) const = default;
};

struct Pose {
    std::array<Keypoint, kJointCount> points{};
    Keypoint& operator[](Joint j) { return points[static_cast<std::size_t>(j)]; }
    const Keypoint& operator[](Joint j) const { return points[static_cast<std::size_t>(j)]; }
    bool operator==(const Pose&) const = default;

    // Mirrors left/right (the figure faces the other way).
    Pose mirrored() const;
    // Lowest visible ankle (y, normalised) — the ground contact.
    double groundY() const;
    // Moves and scales the figure (about the image origin).
    Pose transformed(double scale, double dx, double dy) const;
};

// Bones as joint pairs, in drawing order (matches the conditioning convention).
const std::vector<std::pair<Joint, Joint>>& poseLimbs();
Rgba8 limbColor(std::size_t limbIndex) noexcept;
Rgba8 jointColor(Joint j) noexcept;

// The ControlNet conditioning image: black background, limbs drawn as
// translucent capsules, joints as discs; widths scale with the image size.
Image renderPose(const Pose& pose, int width, int height);

// A standing figure facing right, feet at y = 0.9, about 0.8 tall.
Pose standingPose();

enum class PoseCycle { Idle, Walk, Run, Jump };
std::string_view toString(PoseCycle c) noexcept;
std::optional<PoseCycle> poseCycleFromString(std::string_view s) noexcept;
// Pose at phase t in [0, 1) of a cycle (side view, facing right). Feet stay
// on the ground line except while jumping.
Pose cyclePose(PoseCycle cycle, double t);
// `frames` evenly spaced poses of a cycle.
std::vector<Pose> cyclePoses(PoseCycle cycle, int frames);

// One crop box (normalised x0, y0, x1, y1) that frames every pose, padded by
// `margin` (fraction of the figure height) and grown to `aspect` (w/h),
// clamped to the image. The same box for all frames keeps scale and ground line.
std::array<double, 4> poseFramingBox(const std::vector<Pose>& poses, double aspect, double margin = 0.12);

nlohmann::json poseToJson(const Pose& p);
Result<Pose> poseFromJson(const nlohmann::json& j);

} // namespace acestudio
