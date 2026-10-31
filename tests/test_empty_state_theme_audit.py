import sys
from pathlib import Path
from collections import Counter
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from qa_empty_state_themes import verify_pixels

class PixelIdentity(unittest.TestCase):
    def setUp(self):
        self.check={'fg':[250,250,250,255],'backgrounds':[[20,20,20,255]]*4}

    def test_real_text(self):
        self.assertGreater(verify_pixels(self.check,Counter({(20,20,20):100,(250,250,250):8})),4.5)

    def test_dark_border_is_not_text(self):
        with self.assertRaises(ValueError):
            verify_pixels(self.check,Counter({(20,20,20):100,(0,0,0):10}))

    def test_stale_light_theme_rejected(self):
        with self.assertRaises(ValueError):
            verify_pixels(self.check,Counter({(240,240,240):100,(10,10,10):10}))

    def test_one_matching_pixel_is_insufficient(self):
        with self.assertRaises(ValueError):
            verify_pixels(self.check,Counter({(20,20,20):100,(250,250,250):1}))

if __name__=='__main__': unittest.main()
