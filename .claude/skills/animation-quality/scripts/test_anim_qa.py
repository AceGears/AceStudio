"""Self-tests for anim_qa.py (stdlib unittest, synthetic images only).

Run:  python -I .claude/skills/animation-quality/scripts/test_anim_qa.py -v
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anim_qa  # noqa: E402
from imgio import Image  # noqa: E402

CLEAR = (0, 0, 0, 0)
BODY = (200, 60, 40, 255)
HEAD = (240, 200, 60, 255)
EYE = (20, 20, 20, 255)


def blank(w=24, h=24):
    return Image(w, h, [CLEAR] * (w * h))


def rect(img, x, y, w, h, c):
    for yy in range(y, y + h):
        for xx in range(x, x + w):
            if 0 <= xx < img.w and 0 <= yy < img.h:
                img.px[yy * img.w + xx] = c
    return img


def creature(head_dy=0, head=True, body_dx=0, arm=0):
    """A 'creature': a body block, a head block on top (with an eye), an arm that moves."""
    img = blank()
    rect(img, 6 + body_dx, 10, 12, 10, BODY)
    rect(img, 8 + arm, 20, 3, 3, BODY)          # a moving limb below the body
    if head:
        rect(img, 9, 3 + head_dy, 6, 6, HEAD)
        rect(img, 12, 5 + head_dy, 1, 1, EYE)
    return img


class PingPongTests(unittest.TestCase):
    def test_palindromic_cycle_is_detected(self):
        poses = [creature(arm=a) for a in (0, 1, 2, 3, 4)]
        frames = poses + poses[-2:0:-1]          # 0 1 2 3 4 3 2 1  (n = 8: frame i == frame 8-i)
        pp = anim_qa.pingpong(frames)
        self.assertTrue(pp["is_pingpong"])
        self.assertEqual(pp["pairs_matched"], [(1, 7), (2, 6), (3, 5)])
        self.assertEqual(pp["unique_poses"], 5)
        rep = anim_qa.analyze(frames, "body")
        self.assertTrue(any("ping-pong" in w for w in rep["warnings"]))

    def test_forward_cycle_is_not_pingpong(self):
        frames = [creature(arm=a, body_dx=a % 2) for a in range(6)]   # 0 1 2 3 4 5, back to 0 by looping
        pp = anim_qa.pingpong(frames)
        self.assertFalse(pp["is_pingpong"])
        self.assertEqual(pp["pairs_matched"], [])
        rep = anim_qa.analyze(frames, "body")
        self.assertFalse(any("ping-pong" in w for w in rep["warnings"]))

    def test_partial_symmetry_is_reported_but_not_flagged(self):
        frames = [creature(arm=a) for a in (0, 1, 2, 3, 4, 5)]
        frames[5] = creature(arm=1)              # only the pair (1, 5) matches; (2, 4) does not
        pp = anim_qa.pingpong(frames)
        self.assertEqual(pp["pairs_matched"], [(1, 5)])
        self.assertFalse(pp["is_pingpong"])

    def test_too_short_is_never_pingpong(self):
        frames = [creature(arm=0), creature(arm=1), creature(arm=0)]
        self.assertFalse(anim_qa.pingpong(frames)["is_pingpong"])


class TopRegionTests(unittest.TestCase):
    def test_head_moving_with_body_is_present(self):
        frames = [creature(head_dy=d, arm=i % 3) for i, d in enumerate((0, 1, 2, 1, 0, -1))]
        tr = anim_qa.top_region_presence(frames)
        self.assertTrue(tr["available"])
        self.assertEqual(tr["frames_lost"], [])
        self.assertEqual(tr["min_presence"], 1.0)
        rep = anim_qa.analyze(frames, "body")
        self.assertFalse(any("top-most part" in w for w in rep["warnings"]))

    def test_lost_head_is_flagged_with_frame_number(self):
        frames = [creature(arm=i % 3) for i in range(6)]
        frames[3] = creature(head=False, arm=0)
        tr = anim_qa.top_region_presence(frames)
        self.assertEqual(tr["frames_lost"], [3])
        self.assertEqual(tr["worst_frame"], 3)
        rep = anim_qa.analyze(frames, "body")
        self.assertTrue(any("top-most part" in w and "[3]" in w for w in rep["warnings"]))

    def test_squashed_head_is_flagged(self):
        frames = [creature(arm=i % 3) for i in range(4)]
        squashed = creature(head=False, arm=0)
        rect(squashed, 9, 7, 6, 2, HEAD)         # head flattened to a third of its height
        frames[2] = squashed
        tr = anim_qa.top_region_presence(frames)
        self.assertIn(2, tr["frames_lost"])
        self.assertLess(tr["min_presence"], 0.6)

    def test_small_loss_is_tolerated(self):
        frames = [creature(arm=i % 3) for i in range(4)]
        nicked = creature(arm=0)
        rect(nicked, 9, 3, 6, 1, CLEAR)          # top row of the head gone: 1/6 of it (< 40 %)
        frames[1] = nicked
        tr = anim_qa.top_region_presence(frames)
        self.assertEqual(tr["frames_lost"], [])
        self.assertLess(tr["min_presence"], 1.0)

    def test_effects_skip_the_guard(self):
        frames = [creature(arm=i % 3) for i in range(4)]
        frames[2] = creature(head=False)
        rep = anim_qa.analyze(frames, "effect")
        self.assertNotIn("top_region", rep)


class ExistingMetricsSmokeTests(unittest.TestCase):
    def test_mirror_pair_and_seam(self):
        a = creature(arm=0, body_dx=-2)
        b = Image(a.w, a.h, [p for y in range(a.h) for p in a.px[y * a.w:(y + 1) * a.w][::-1]])
        rep = anim_qa.analyze([a, b, a, b], "body")
        self.assertTrue(any("horizontal mirrors" in w for w in rep["warnings"]))

    def test_clean_cycle_has_no_new_warnings(self):
        frames = [creature(arm=a, head_dy=a % 2) for a in (0, 1, 2, 3)]
        rep = anim_qa.analyze(frames, "body")
        self.assertFalse(rep["pingpong"]["is_pingpong"])
        self.assertEqual(rep["top_region"]["frames_lost"], [])


if __name__ == "__main__":
    unittest.main()
