import importlib.util
import unittest
from pathlib import Path
import numpy as np
spec = importlib.util.spec_from_file_location('cmp_ref', Path(__file__).with_name('cmp_ref.py'))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

class CompareTests(unittest.TestCase):
    def test_black_frames_fail(self):
        self.assertFalse(m.acceptable(np.array([0, 0, 0]), np.array([1000, 1200, 500])))
    def test_fixed_tolerances(self):
        self.assertTrue(m.acceptable(np.array([1000, 1200, 500]), np.array([1000, 1200, 500])))
        self.assertFalse(m.acceptable(np.array([1000, 600, 500]), np.array([1000, 1200, 500])))
        self.assertFalse(m.acceptable(np.array([1000, 1200, 1000]), np.array([1000, 1200, 500])))

if __name__ == '__main__': unittest.main()
