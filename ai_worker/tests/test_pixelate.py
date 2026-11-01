import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

try:
    from PIL import Image, ImageDraw
except ImportError:   # Pillow is part of the AI runtime, not of every CI image
    Image = None


@unittest.skipIf(Image is None, "Pillow not installed")
class PixelateTest(unittest.TestCase):
    def test_transparent_input_keeps_hard_alpha_and_is_framed(self):
        from acestudio_worker.tasks.generate import pixelate
        im = Image.new("RGBA", (1024, 1024), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        d.ellipse((300, 200, 700, 900), fill=(200, 50, 50, 255))
        out = pixelate(im, 32, 32, colors=8, crop=True)
        self.assertEqual(out.size, (32, 32))
        self.assertEqual(set(out.getchannel("A").getdata()), {0, 255})
        x0, y0, x1, y1 = out.getchannel("A").getbbox()
        self.assertLessEqual(y0, 3)          # framed: the subject fills the height
        self.assertGreaterEqual(y1, 29)

    def test_opaque_input_unchanged_behaviour(self):
        from acestudio_worker.tasks.generate import pixelate
        im = Image.new("RGB", (256, 256), (255, 255, 255))
        out = pixelate(im, 16, 16, colors=4)
        self.assertEqual(out.mode, "RGBA")
        self.assertEqual(out.getchannel("A").getextrema(), (255, 255))


if __name__ == "__main__":
    unittest.main()
