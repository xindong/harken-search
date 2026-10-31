"""Numerical and coverage contracts for the real transport audit."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
PATH = ROOT / 'tools/qa_transport_themes.py'
SPEC = importlib.util.spec_from_file_location('qa_transport_themes', PATH) if PATH.exists() else None
audit = importlib.util.module_from_spec(SPEC) if SPEC else None
if SPEC:
    SPEC.loader.exec_module(audit)


class TransportAuditTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(audit, 'The reusable transport audit entry is missing')

    def test_known_contrast_and_alpha_composition(self):
        self.assertAlmostEqual(audit.contrast([0, 0, 0, 255], [255, 255, 255, 255]), 21)
        self.assertAlmostEqual(audit.contrast([255, 255, 255, 0], [14, 61, 72, 255]), 1)
        self.assertLess(audit.contrast([1, 64, 95, 122], [14, 61, 72, 255]), 1.04)

    def test_threshold_is_not_rounded_before_judgment(self):
        self.assertEqual(audit.classify(4.4999, 'text', False), 'fail')
        self.assertEqual(audit.classify(4.5, 'text', False), 'pass')
        self.assertEqual(audit.classify(2.9999, 'graphic', False), 'fail')
        self.assertEqual(audit.classify(1.0, 'text', True), 'disabled')

    def test_missing_or_clipped_controls_are_not_passed(self):
        snap = {'theme': 5, 'frame': 12, 'bar': [0, 0, 800, 36], 'checks': []}
        result = audit.analyze_snapshot(snap, required={'eq.label'})
        self.assertEqual(result['missing'], ['eq.label'])
        snap['checks'] = [{'id': 'eq.label', 'kind': 'text', 'fg': [255]*4,
                           'bg': [0, 0, 0, 255], 'bounds': [790, 0, 820, 20], 'disabled': False}]
        result = audit.analyze_snapshot(snap, required={'eq.label'})
        self.assertEqual(result['checks'][0]['status'], 'clipped')

    def test_bypass_is_still_an_operable_control(self):
        snap = {'theme': 5, 'frame': 12, 'bar': [0, 0, 800, 36], 'checks': [
            {'id': 'eq.label', 'kind': 'text', 'fg': [1, 64, 95, 122],
             'bg': [14, 61, 72, 255], 'bounds': [100, 4, 140, 28], 'disabled': False}]}
        self.assertEqual(audit.analyze_snapshot(snap, required={'eq.label'})['checks'][0]['status'], 'fail')

    def test_text_inside_bar_but_outside_its_clip_is_rejected(self):
        row = {'id': 'eq.slope', 'kind': 'text', 'fg': [255]*4,
               'bg': [0, 0, 0, 255], 'bounds': [100, 5, 125, 21],
               'clip': [90, 0, 150, 13], 'bounds_kind': 'glyphs', 'disabled': False}
        snap = {'bar': [0, 0, 800, 36], 'checks': [row]}
        self.assertEqual(audit.analyze_snapshot(snap, required=set())['checks'][0]['status'], 'clipped')
        row['bounds'] = [100, 5, 125, 12]
        self.assertEqual(audit.analyze_snapshot(snap, required=set())['checks'][0]['status'], 'pass')

    def test_control_box_does_not_claim_internal_text_coverage(self):
        row = {'id': 'database.label', 'kind': 'text', 'fg': [255]*4,
               'bg': [0, 0, 0, 255], 'bounds': [100, 5, 125, 21], 'disabled': False}
        result = audit.analyze_snapshot({'bar': [0, 0, 800, 36], 'checks': [row]}, required=set())
        self.assertEqual(result['checks'][0].get('text_clipping_measured'), False)

    def test_invalid_colors_are_rejected(self):
        with self.assertRaises(ValueError):
            audit.contrast([float('nan'), 0, 0, 255], [0, 0, 0, 255])
        with self.assertRaises(ValueError):
            audit.contrast([0, 0, 0, 255], [0, 0, 0, 40])

    def test_conditional_controls_must_be_present(self):
        snap = {'spectrum': True, 'spectrum_collapse': 0, 'selection': False}
        required = audit.required_for('spectrum', snap)
        self.assertIn('spectrum.fft', required)
        self.assertIn('spectrum.blend', required)
        snap['spectrum_collapse'] = 2
        self.assertIn('spectrum.overflow', audit.required_for('spectrum', snap))
        self.assertNotIn('spectrum.fft', audit.required_for('spectrum', snap))
        with self.assertRaises(ValueError):
            audit.required_for('selection', snap)


if __name__ == '__main__':
    unittest.main()
