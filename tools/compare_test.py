import importlib.util
import unittest
from pathlib import Path
import numpy as np
spec = importlib.util.spec_from_file_location('cmp_ref', Path(__file__).with_name('cmp_ref.py'))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

raw_spec = importlib.util.spec_from_file_location('cmp_raw_ref', Path(__file__).with_name('cmp_raw_ref.py'))
raw = importlib.util.module_from_spec(raw_spec); raw_spec.loader.exec_module(raw)
from PIL import Image

class CompareTests(unittest.TestCase):
    def test_raw_exactness_and_dimensions(self):
        a = Image.new('RGB', (2, 2)); b = a.copy()
        self.assertEqual(raw.difference(a, b), 0)
        b.putpixel((1, 1), (0, 1, 0)); self.assertEqual(raw.difference(a, b), 1)
        self.assertIsNone(raw.difference(a, Image.new('RGB', (1, 4))))

    def test_black_frames_fail(self):
        self.assertFalse(m.acceptable(np.array([0, 0, 0]), np.array([1000, 1200, 500])))
    def test_fixed_tolerances(self):
        self.assertTrue(m.acceptable(np.array([1000, 1200, 500]), np.array([1000, 1200, 500])))
        self.assertFalse(m.acceptable(np.array([1000, 600, 500]), np.array([1000, 1200, 500])))
        self.assertFalse(m.acceptable(np.array([1000, 1200, 1000]), np.array([1000, 1200, 500])))

if __name__ == '__main__': unittest.main()
