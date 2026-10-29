#pragma once

// Playback order for animation tags, shared by the timeline, the preview
// window and the exporters so they always agree.

#include "acestudio/core/Document.h"

#include <optional>
#include <vector>

namespace acestudio {

// One cycle of a tag in playback order:
//   forward  from..to
//   reverse  to..from
//   pingpong from..to..from+1 (ends are not repeated, so it loops cleanly)
std::vector<int> tagCycle(const AnimationTag& tag);

// Full playback sequence: the cycle repeated `tag.repeat` times, or once
// when repeat is 0 (loop forever — callers loop the result).
std::vector<int> tagSequence(const AnimationTag& tag);

// Index of the first tag that contains `frame`, if any.
std::optional<std::size_t> tagAt(const Document& doc, int frame);

// Steps through a document's frames honouring a tag (or all frames when
// no tag applies) and each frame's duration.
class AnimationPlayer {
public:
    // Plays the tag containing `startFrame`, or every frame if none.
    void reset(const Document& doc, int startFrame);
    int currentFrame() const noexcept { return m_sequence.empty() ? 0 : m_sequence[m_pos]; }
    // Advances one step; returns false when a finite repeat count is done.
    bool advance() noexcept;
    bool finished() const noexcept { return m_finished; }
    const std::vector<int>& sequence() const noexcept { return m_sequence; }

private:
    std::vector<int> m_sequence;
    std::size_t m_pos = 0;
    bool m_loopForever = true;
    bool m_finished = false;
};

} // namespace acestudio
