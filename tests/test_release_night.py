"""Reject night fixture inputs that could hide the unproved life cycle."""
import importlib.util
import json
import os
import subprocess
import tempfile
import sys
from pathlib import Path
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("night_fixture", ROOT / "tools/stage_night_fixture.py")
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


def rows():
    result = []
    for cls, item in zip(("", fixture.CLASS, "/Game/GCrackPotS.GCrackPotS_C"), fixture.specs([-377,1371,210])):
        result.append({"id":len(result)+1, "class": cls, "init_position":item["position"], "position": item["position"], "nav_position": item["position"],
            "sublevel": "Area001", "enable_save": True, "save_flag": "EGeneratorActorSaveFlag::SaveNum",
            "rebirth": "ERebirthType::NoRebirth"})
    return result


class NightFixture(unittest.TestCase):
    def test_foreign_output_is_refused_before_reading_or_changing_inputs(self):
        self.assertEqual(fixture.OUTPUT, ROOT / "build/night-fixture")
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary)
            # Inputs from another checkout cannot grant ownership of its output.
            old_tools, foreign = home / "source/tools", home / "private/night-fixture"
            with mock.patch.object(Path, "read_text") as read, \
                    mock.patch.object(Path, "mkdir") as mkdir, \
                    mock.patch.object(fixture.subprocess, "run") as convert:
                with self.assertRaisesRegex(ValueError, "single scoped build/night-fixture"):
                    fixture.stage(old_tools, home / "donor.uasset", home / "reference.log", foreign)
                read.assert_not_called()
                mkdir.assert_not_called()
                convert.assert_not_called()

    def test_owned_output_name_cannot_resolve_outside_its_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary).resolve()
            owned, foreign = home / "build/night-fixture", home / "foreign"
            resolve = Path.resolve

            def redirected(path, *args, **kwargs):
                return foreign if path == owned else resolve(path, *args, **kwargs)

            with mock.patch.object(fixture, "OUTPUT", owned), \
                    mock.patch.object(Path, "resolve", redirected), \
                    mock.patch.object(Path, "read_text") as read:
                with self.assertRaisesRegex(ValueError, "single scoped build/night-fixture"):
                    fixture.stage(home / "tools", home / "donor.uasset", home / "reference.log", owned)
                read.assert_not_called()

    def test_packing_canonicalizes_short_and_long_parent_spellings(self):
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary).resolve()
            actual, alias = home / "long fixture directory", home / "FIXTUR~1"
            stage, target, converter = actual / "stage", actual / "out/Paks", actual / "retoc.exe"
            converter.parent.mkdir(parents=True)
            converter.write_bytes(b"converter")
            for suffix in (".uasset", ".uexp"):
                asset = stage / ("Carrot4/Content/P4Mods/release_t3_night/AP_Area001_P_ReleaseT3Night" + suffix)
                asset.parent.mkdir(parents=True, exist_ok=True)
                asset.write_bytes(b"owned package")
            resolve = Path.resolve

            def canonical(path, *args, **kwargs):
                # Model Windows resolving an 8.3 path without requiring a
                # particular short-name policy on the developer's volume.
                try:
                    return resolve(actual / path.relative_to(alias), *args, **kwargs)
                except ValueError:
                    return resolve(path, *args, **kwargs)

            def converted(command, **kwargs):
                self.assertEqual(command[:3], [str(converter.resolve()), "to-zen", str(stage.resolve())])
                for suffix in (".pak", ".ucas", ".utoc"):
                    Path(command[3]).with_suffix(suffix).write_bytes(b"converted")

            with mock.patch.object(Path, "resolve", canonical), \
                    mock.patch.object(fixture.subprocess, "run", side_effect=converted):
                result = fixture.pack_stage(alias / "stage", alias / "out/Paks", alias / "retoc.exe")
            self.assertEqual(result, target)
            self.assertEqual({p.read_bytes() for p in target.iterdir()}, {b"converted"})

    def test_packing_still_refuses_output_links_and_paths_outside_the_scope(self):
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary).resolve()
            stage, target = home / "stage", home / "out/Paks"
            stage.mkdir()
            original = Path.is_symlink

            def linked(path):
                return path == target.parent or original(path)

            with mock.patch.object(Path, "is_symlink", linked), \
                    mock.patch.object(fixture.subprocess, "run") as convert:
                with self.assertRaisesRegex(ValueError, "filesystem link"):
                    fixture.pack_stage(stage, target, home / "retoc.exe")
                convert.assert_not_called()
            with mock.patch.object(fixture.subprocess, "run") as convert:
                with self.assertRaises(ValueError):
                    fixture.pack_stage(stage, home.parent / "foreign/Paks", home / "retoc.exe")
                convert.assert_not_called()

    def test_night_targets_the_actual_surface_world_and_owns_its_save_mark(self):
        manifest = json.loads((ROOT / "tests/fixtures/release_night/release.t3.night/mod.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["maps"], [{"map":"/Game/Carrot4/Maps/Main/Area/Area001/Area001_P",
            "mode":"night", "placements":[fixture.PACKAGE]}])
        self.assertNotEqual(fixture.runtime_mark(), 229783852862470387)
        self.assertGreater(fixture.runtime_mark(), 0)

    def test_native_combat_waits_for_settled_donor_and_uses_only_native_glow_squad(self):
        runner = os.environ.get("T3_NIGHT_LUA_HOST") or ROOT / "build" / ("lua_host.exe" if sys.platform == "win32" else "lua_host")
        result = subprocess.run([str(runner), str(ROOT / "tests/fixtures/release_night/combat_test.lua")],
                                cwd=ROOT, capture_output=True, text=True, check=True, timeout=30)
        self.assertIn("night native combat is guarded, paced and releases its cursor", result.stdout)

    def test_observer_separates_absence_defeat_and_handle_expiry(self):
        runner = os.environ.get("T3_NIGHT_LUA_HOST") or ROOT / "build" / ("lua_host.exe" if sys.platform == "win32" else "lua_host")
        result = subprocess.run([str(runner), str(ROOT / "tests/fixtures/release_night/observer_test.lua")],
                                cwd=ROOT, capture_output=True, text=True, check=True, timeout=30)
        self.assertIn("night observer separates absence, defeat and handle expiry", result.stdout)

    def test_reentry_requires_exact_doctor_and_captain_readback(self):
        runner = os.environ.get("T3_NIGHT_LUA_HOST") or ROOT / "build" / ("lua_host.exe" if sys.platform == "win32" else "lua_host")
        result = subprocess.run([str(runner), str(ROOT / "tests/fixtures/release_night/reentry_test.lua")],
                                cwd=ROOT, capture_output=True, text=True, check=True, timeout=30)
        self.assertIn("night reentry uses exact Doctor guards, captain-only setup and readback", result.stdout)

    def test_recorded_native_surface_position_is_required(self):
        text = " 1860 actor GAvatar_19 kind AVATAR placed 0 at -377 1371 210\n"
        self.assertEqual(fixture.position_from_log(text)["position"], [-377,1371,210])
        for text in ("", "1860 actor GAvatar_19 kind AVATAR placed 0 at 700 -1000 -85\n"):
            with self.assertRaises(ValueError):
                fixture.position_from_log(text)

    def test_mark_save_policy_scope_and_nav_anchor_are_required(self):
        ids = {"mark": 1, "larva": 2, "pot": 3}
        result = fixture.validate(rows(), ids, [-377,1371,210])
        self.assertFalse(result["game_proof"])
        for field, value in (("id", 99), ("init_position", [700,-1000,-85]), ("class", "/Game/Other"), ("sublevel", "Cave000_F00"),
                             ("enable_save", False), ("save_flag", "None"), ("rebirth", "Allday"),
                             ("nav_position", [700,-1000,-85])):
            changed = rows()
            changed[1][field] = value
            with self.assertRaises(ValueError, msg=field):
                fixture.validate(changed, ids, [-377,1371,210])
        with self.assertRaises(ValueError):
            fixture.validate(rows(), {"mark":1,"larva":1,"pot":3}, [-377,1371,210])

    def test_packing_uses_the_converter_and_preserves_previous_output_on_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary)
            stage, target, converter = home / "stage", home / "out/Paks", home / "retoc.exe"
            converter.write_bytes(b"converter")
            for suffix in (".uasset", ".uexp"):
                asset = stage / ("Carrot4/Content/P4Mods/release_t3_night/AP_Area001_P_ReleaseT3Night" + suffix)
                asset.parent.mkdir(parents=True, exist_ok=True)
                asset.write_bytes(b"owned package")
            target.mkdir(parents=True)
            for suffix in (".pak", ".ucas", ".utoc"):
                (target / ("ReleaseT3Night_P" + suffix)).write_bytes(b"previous")

            def converted(command, **kwargs):
                self.assertEqual(command[:3], [str(converter.resolve()), "to-zen", str(stage.resolve())])
                self.assertEqual(command[4:], ["--version", "UE4_26"])
                self.assertTrue(kwargs["check"])
                output = Path(command[3])
                self.assertEqual(output.name, "ReleaseT3Night_P.utoc")
                for suffix in (".pak", ".ucas", ".utoc"):
                    output.with_suffix(suffix).write_bytes(b"converted")

            with mock.patch.object(fixture.subprocess, "run", side_effect=subprocess.CalledProcessError(1, "retoc")):
                with self.assertRaises(subprocess.CalledProcessError):
                    fixture.pack_stage(stage, target, converter)
            self.assertEqual({p.read_bytes() for p in target.iterdir()}, {b"previous"})
            with mock.patch.object(fixture.subprocess, "run"):
                with self.assertRaises(ValueError):
                    fixture.pack_stage(stage, target, converter)
            self.assertEqual({p.read_bytes() for p in target.iterdir()}, {b"previous"})
            unrelated = target / "other.txt"
            unrelated.write_bytes(b"other")
            with mock.patch.object(fixture.subprocess, "run") as packed:
                with self.assertRaises(ValueError):
                    fixture.pack_stage(stage, target, converter)
                packed.assert_not_called()
            unrelated.unlink()
            stock = stage / "Carrot4/Content/Stock.uasset"
            stock.write_bytes(b"stock")
            with mock.patch.object(fixture.subprocess, "run") as packed:
                with self.assertRaises(ValueError):
                    fixture.pack_stage(stage, target, converter)
                packed.assert_not_called()
            stock.unlink()
            rename = Path.rename

            def cannot_publish(path, destination):
                if path.name == "Paks" and path.parent.name.startswith("night-pack-"):
                    raise OSError("publication refused")
                return rename(path, destination)

            with mock.patch.object(fixture.subprocess, "run", side_effect=converted), \
                    mock.patch.object(Path, "rename", cannot_publish):
                with self.assertRaises(OSError):
                    fixture.pack_stage(stage, target, converter)
            self.assertEqual({p.read_bytes() for p in target.iterdir()}, {b"previous"})
            with mock.patch.object(fixture.subprocess, "run", side_effect=converted):
                fixture.pack_stage(stage, target, converter)
            self.assertEqual({p.read_bytes() for p in target.iterdir()}, {b"converted"})
