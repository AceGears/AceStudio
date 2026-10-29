#include "acestudio/core/Animation.h"

#include <algorithm>

namespace acestudio {

std::vector<int> tagCycle(const AnimationTag& t) {
    std::vector<int> out;
    if (t.toFrame < t.fromFrame) return out;
    switch (t.direction) {
    case PlaybackDirection::Forward:
        for (int f = t.fromFrame; f <= t.toFrame; ++f) out.push_back(f);
        break;
    case PlaybackDirection::Reverse:
        for (int f = t.toFrame; f >= t.fromFrame; --f) out.push_back(f);
        break;
    case PlaybackDirection::PingPong:
        for (int f = t.fromFrame; f <= t.toFrame; ++f) out.push_back(f);
        for (int f = t.toFrame - 1; f > t.fromFrame; --f) out.push_back(f);
        break;
    }
    return out;
}

std::vector<int> tagSequence(const AnimationTag& t) {
    const auto cycle = tagCycle(t);
    const int loops = std::max(1, t.repeat);
    std::vector<int> out;
    out.reserve(cycle.size() * static_cast<std::size_t>(loops) + 1);
    for (int i = 0; i < loops; ++i) out.insert(out.end(), cycle.begin(), cycle.end());
    // A finite ping-pong ends back on its first frame.
    if (t.direction == PlaybackDirection::PingPong && t.repeat > 0 && t.toFrame > t.fromFrame) out.push_back(t.fromFrame);
    return out;
}

std::optional<std::size_t> tagAt(const Document& doc, int frame) {
    const auto& tags = doc.tags();
    for (std::size_t i = 0; i < tags.size(); ++i)
        if (frame >= tags[i].fromFrame && frame <= tags[i].toFrame) return i;
    return std::nullopt;
}

void AnimationPlayer::reset(const Document& doc, int startFrame) {
    m_sequence.clear();
    m_pos = 0;
    m_finished = false;
    if (auto t = tagAt(doc, startFrame)) {
        const AnimationTag& tag = doc.tags()[*t];
        m_sequence = tagSequence(tag);
        m_loopForever = tag.repeat == 0;
        // Start from the requested frame's first occurrence.
        auto it = std::find(m_sequence.begin(), m_sequence.end(), startFrame);
        if (it != m_sequence.end()) m_pos = static_cast<std::size_t>(it - m_sequence.begin());
    } else {
        for (int f = 0; f < doc.frameCount(); ++f) m_sequence.push_back(f);
        m_loopForever = true;
        m_pos = static_cast<std::size_t>(std::clamp(startFrame, 0, std::max(0, doc.frameCount() - 1)));
    }
}

bool AnimationPlayer::advance() noexcept {
    if (m_sequence.empty() || m_finished) return false;
    if (m_pos + 1 < m_sequence.size()) {
        ++m_pos;
        return true;
    }
    if (m_loopForever) {
        m_pos = 0;
        return true;
    }
    m_finished = true;
    return false;
}

} // namespace acestudio
