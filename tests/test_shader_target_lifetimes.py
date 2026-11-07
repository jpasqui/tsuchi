"""Actual linked renderer/material custody and parameter-copy host checks."""
import unittest
import test_all as checks


class ShaderTargetLifetimes(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        checks.Generated.setUpClass()

    def test_renderer_custody_parameter_copies_and_reentry(self):
        program = checks.Generated().objects_program("t4-objects")
        checks.run([program, checks.ROOT / "tests/game/objects.lua", "--shader-targets"])


if __name__ == "__main__":
    unittest.main()
