"""Regression tests for the exported G-code auditor (stdlib only)."""
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from continuous_print_check import analyse, parse


class ContinuousPrintCheckTests(unittest.TestCase):
    def check_gcode(self, text, **kwargs):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'test.gcode'
            path.write_text(text, encoding='utf-8')
            with contextlib.redirect_stdout(io.StringIO()):
                return analyse(str(path), **kwargs), parse(str(path))

    def test_flat_layers_skip_setup_and_keep_modal_roles(self):
        code, layers = self.check_gcode('''M83
G1 X100 E1
;LAYER_CHANGE
;TYPE:Skirt
G1 X10 E1
G1 X0
;TYPE:Outer wall
G1 Z0.2
G1 X10 E1
;LAYER_CHANGE
G1 Z0.4
G1 Y10 E1
''')
        self.assertEqual(code, 0)
        self.assertEqual([len(layer.extrusions) for layer in layers], [0, 1, 1])

    def test_ramp_is_rejected_unless_explicitly_allowed(self):
        text = ';LAYER_CHANGE\nM83\nG1 Z0.2\nG1 X10 Z0.25 E1\nG1 Y10 Z0.4 E1\n'
        self.assertEqual(self.check_gcode(text)[0], 1)
        self.assertEqual(self.check_gcode(text, allow_z_ramp=True)[0], 0)

    def test_absolute_extrusion_reset_and_arc_are_recognized(self):
        code, layers = self.check_gcode('''M82
;LAYER_CHANGE
G1 Z0.2
G1 X10 E5
G92 E0
G3 X20 Y10 I0 J10 E1
''')
        self.assertEqual(code, 0)
        self.assertEqual(len(layers[1].extrusions), 2)

    def test_boundary_excursion_is_detected_even_if_it_returns_to_start(self):
        code, _ = self.check_gcode('''M83
;LAYER_CHANGE
G1 Z0.2
G1 X10 E1
G1 X11
;LAYER_CHANGE
G1 Z0.4
G1 X10
G1 Y10 E1
''')
        self.assertEqual(code, 1)

    def test_relative_xyz_respects_g92(self):
        code, layers = self.check_gcode('''M83
;LAYER_CHANGE
G1 Z0.2
G92 X10 Y20
G91
G1 X1 E1
G1 Y2 E1
''')
        self.assertEqual(code, 0)
        self.assertEqual((layers[1].extrusions[-1].x1, layers[1].extrusions[-1].y1), (11, 22))


if __name__ == '__main__':
    unittest.main()
