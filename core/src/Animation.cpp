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

namespace {
struct Ghost { int frame; int distance; bool before; };

std::vector<Ghost> ghostsFor(const Document& doc, int frame, const OnionSkin& o) {
    std::vector<Ghost> ghosts;
    const int n = doc.frameCount();
    if (n < 2 || o.opacity <= 0) return ghosts;
    auto add = [&](int f, int dist, bool before) {
        if (o.wrap) f = ((f % n) + n) % n;
        if (f < 0 || f >= n || f == frame) return;
        ghosts.push_back({f, dist, before});
    };
    for (int d = 1; d <= std::max(0, o.before); ++d) add(frame - d, d, true);
    for (int d = 1; d <= std::max(0, o.after); ++d) add(frame + d, d, false);
    // Furthest first so nearer ghosts end up on top.
    std::stable_sort(ghosts.begin(), ghosts.end(), [](const Ghost& a, const Ghost& b) { return a.distance > b.distance; });
    return ghosts;
}
} // namespace

std::vector<int> onionSkinFrames(const Document& doc, int frame, const OnionSkin& o) {
    std::vector<int> out;
    for (const Ghost& g : ghostsFor(doc, frame, o)) out.push_back(g.frame);
    return out;
}

Image renderOnionSkin(const Document& doc, int frame, const OnionSkin& o, const std::function<const Image&(int)>& composite) {
    Image out(doc.width(), doc.height());
    Image scratch;
    for (const Ghost& g : ghostsFor(doc, frame, o)) {
        const Image* src = nullptr;
        if (composite) src = &composite(g.frame);
        if (!src || src->width() != doc.width() || src->height() != doc.height()) {
            scratch = doc.composite(g.frame);
            src = &scratch;
        }
        const int pct = std::clamp(o.opacity / g.distance, 1, 100);
        const auto opacity = static_cast<std::uint8_t>(pct * 255 / 100);
        if (o.tint) {
            Image img = *src;
            const Rgba8 t = g.before ? Rgba8{220, 60, 60, 255} : Rgba8{60, 110, 230, 255};
            for (auto& p : img.pixels())
                if (p.a) p = {static_cast<std::uint8_t>((p.r + t.r) / 2), static_cast<std::uint8_t>((p.g + t.g) / 2),
                              static_cast<std::uint8_t>((p.b + t.b) / 2), p.a};
            blendImage(out, img, 0, 0, BlendMode::Normal, opacity);
        } else {
            blendImage(out, *src, 0, 0, BlendMode::Normal, opacity);
        }
    }
    return out;
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
        resetAll(doc, startFrame);
    }
}

void AnimationPlayer::resetAll(const Document& doc, int startFrame) {
    m_sequence.clear();
    m_finished = false;
    for (int f = 0; f < doc.frameCount(); ++f) m_sequence.push_back(f);
    m_loopForever = true;
    m_pos = static_cast<std::size_t>(std::clamp(startFrame, 0, std::max(0, doc.frameCount() - 1)));
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
