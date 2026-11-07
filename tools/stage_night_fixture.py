"""Build a private, deterministic night placement donor and a separate observer."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/fixtures/release_night"
OUTPUT = ROOT / "build/night-fixture"
PACKAGE = "/Game/P4Mods/release_t3_night/AP_Area001_P_ReleaseT3Night"
WORLD = "/Game/Carrot4/Maps/Main/Area/Area001/Area001_P"
CLASS = "/Game/Carrot4/Placeables/Teki/GBaby.GBaby_C"
SALT = "release.t3.night-v1"


def runtime_mark():
    # The runtime replaces the template's mark number with this path-bound ID.
    text = ("placements\nrelease.t3.night\n" + PACKAGE).lower().encode("utf-8")
    number = int.from_bytes(hashlib.sha256(text).digest()[:8], "little")
    return number if number not in (0, (1 << 64)-1) else 1


def position_from_log(text):
    """Keep the first recorded native captain position, not a cave coordinate."""
    match = re.search(r"^\s*(\d+) actor (GAvatar_\S+) kind AVATAR placed 0 at (-?\d+) (-?\d+) (-?\d+)\s*$", text, re.M)
    if not match:
        raise ValueError("No recorded native captain position")
    position = list(map(int, match.groups()[2:]))
    if position[2] <= 0:
        raise ValueError("The reference is not the qualified surface start")
    return dict(step=int(match[1]), actor=match[2], position=position)


def specs(position):
    x, y, z = position
    return [dict(index=0, position=position, yaw=0),
            dict(index=2, position=position, yaw=0),
            dict(index=1, position=[x+80, y, z], yaw=0)]


def validate(rows, ids, position):
    if len(rows) != 3 or len(set(ids.values())) != 3 or any(value <= 0 for value in ids.values()):
        raise ValueError("A distinct mark, larva, and persistence control are required")
    if rows[0]["class"] not in ("", "None") or rows[1]["class"] != CLASS or "GCrackPotS" not in rows[2]["class"]:
        raise ValueError("The donor mark and classes differ")
    for row, spec, expected_id in zip(rows, specs(position), ids.values()):
        if row["id"] != expected_id:
            raise ValueError("A saved generator ID differs from the declared ID")
        if row["position"] != spec["position"] or row["init_position"] != spec["position"] or row["nav_position"] != spec["position"]:
            raise ValueError("A transform or navigation anchor differs")
        if row["sublevel"] != "Area001" or not row["enable_save"]:
            raise ValueError("A row has the wrong surface scope or save policy")
    if rows[1]["save_flag"] != "EGeneratorActorSaveFlag::SaveNum" or rows[1]["rebirth"] != "ERebirthType::NoRebirth":
        raise ValueError("The larva would not preserve natural defeat")
    return dict(donor_readback=True, game_proof=False)


def pack_stage(stage, target, retoc):
    """Convert only this fixture's new package, then publish all three files."""
    original_target = Path(target).absolute()
    stage, target, retoc = map(lambda p: Path(p).resolve(), (stage, target, retoc))
    target.relative_to(stage.parent)
    # Keep the caller's spelling for link checks. Resolving it first would hide
    # a symlink, but its canonical parents may have Windows short path names.
    for path in (original_target, *original_target.parents):
        if path.is_symlink() or path.exists() and getattr(path.lstat(), "st_file_attributes", 0) & 0x400:
            raise ValueError("The container output contains a filesystem link")
        if path.resolve() == stage.parent:
            break
    if not retoc.is_file():
        raise ValueError("Supply the retoc executable with --retoc")
    package = Path("Carrot4/Content/P4Mods/release_t3_night/AP_Area001_P_ReleaseT3Night")
    expected = {package.with_suffix(s) for s in (".uasset", ".uexp")}
    if {p.relative_to(stage) for p in stage.rglob("*") if p.is_file()} != expected:
        raise ValueError("Pack only the fixture's new package; stock replacements need header redirects")
    names = {"ReleaseT3Night_P" + s for s in (".pak", ".ucas", ".utoc")}
    if target.exists():
        for path in [target, *target.iterdir()]:
            if path.is_symlink() or getattr(path.lstat(), "st_file_attributes", 0) & 0x400:
                raise ValueError("The container output contains a filesystem link")
            if path != target and (path.name not in names or not path.is_file()):
                raise ValueError("The container output contains an unrelated file")
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="night-pack-", dir=target.parent) as temporary:
        prepared = Path(temporary) / "Paks"
        prepared.mkdir()
        subprocess.run([str(retoc), "to-zen", str(stage), str(prepared / "ReleaseT3Night_P.utoc"),
                        "--version", "UE4_26"], check=True, capture_output=True, text=True)
        if any(not (prepared / name).is_file() or not (prepared / name).stat().st_size for name in names):
            raise ValueError("Packaging did not produce all three nonempty container files")
        previous = Path(temporary) / "previous"
        if target.exists():
            target.rename(previous)
        try:
            prepared.rename(target)
        except BaseException:
            if previous.exists():
                previous.rename(target)
            raise
    return target


def stage(old_tools, donor, reference_log, output, pack=False, retoc=None):
    output = Path(output).resolve()
    if output != OUTPUT:
        raise ValueError("Use the single scoped build/night-fixture output folder")
    old_tools, donor, reference_log = map(lambda p: Path(p).resolve(),
                                         (old_tools, donor, reference_log))
    manifest = json.loads((FIXTURE / "release.t3.night/mod.json").read_text(encoding="utf-8"))
    if manifest["maps"] != [{"map": WORLD, "mode": "night", "placements": [PACKAGE]}]:
        raise ValueError("The canonical surface package and explicit night mode are required")
    if pack and (retoc is None or not Path(retoc).is_file()):
        raise ValueError("Supply the retoc executable with --retoc before packing")
    output.mkdir(parents=True, exist_ok=True)
    sys.path[:0] = [str(old_tools), str(old_tools.parent), str(old_tools / "forest_of_hope")]
    from placement_rows import build_list, nested
    from edit_generator_position import entries
    from owned_package import write
    from ue_package import Package
    from ue_tagged import Asset, Reader
    reference = position_from_log(reference_log.read_text(encoding="utf-8"))
    position = reference["position"]
    source = Asset(donor)
    source_rows = entries(source)
    if len(source_rows) != 3 or source_rows[2]["SoftRefActorClass"]["value"]["asset"] != CLASS:
        raise ValueError("Use the qualified three-row cooked GBaby donor")
    working = output / "working/rows.uasset"
    relocated = output / "stage/Carrot4/Content/P4Mods/release_t3_night/AP_Area001_P_ReleaseT3Night.uasset"
    for path in (working, relocated):
        path.parent.mkdir(parents=True, exist_ok=True)
        for suffix in (".uasset", ".uexp"):
            path.with_suffix(suffix).unlink(missing_ok=True)
    built = build_list(donor, specs(position), working, SALT)
    own = Asset(working)
    names = [name.replace("AP_Cave000_Test", PACKAGE.rsplit("/", 1)[1]) for name in own.names]
    write(working, relocated, PACKAGE, names)
    # A typed FName patch puts every row in the actual surface sublevel.
    own = Asset(relocated)
    pkg = Package.load(relocated)
    name = pkg.fname("Area001")
    for row in entries(own):
        prop = row["SubLevelName"]
        if prop["type"] != "NameProperty" or prop["size"] != 8:
            raise ValueError("SubLevelName is not a typed FName")
        pkg.patch_body(prop["offset"], struct.pack("<ii", *name))
    pkg.save(relocated)
    final = Asset(relocated)
    rows = []
    for row in entries(final):
        transform = nested(final, row["Transform"])
        info = nested(final, row["GenerateInfo"])
        rebirth = nested(final, row["RebirthInfo"])
        initial = nested(final, row["InitTransform"])
        rows.append(dict(id=info["DebugUniqueId"]["value"],
            init_position=list(struct.unpack_from("<3f", final.body, initial["Translation"]["offset"])), class_=row["SoftRefActorClass"]["value"]["asset"],
            position=list(struct.unpack_from("<3f", final.body, transform["Translation"]["offset"])),
            nav_position=list(struct.unpack_from("<3f", final.body, row["LastNavPos"]["offset"])),
            sublevel=row["SubLevelName"]["value"], enable_save=info["EnableSave"]["value"],
            save_flag=rebirth["SaveFlag"]["value"], rebirth=rebirth["RebirthType"]["value"]))
    for row in rows:
        row["class"] = row.pop("class_")
    ids = dict(zip(("mark", "larva", "pot"), (item["debug_unique_id"] for item in built)))
    qualification = validate(rows, ids, position)
    mods = output / "out/romfs/tsuchi/mods"
    for name in ("release.t3.night", "release.t3.night.observer"):
        home = mods / name
        home.mkdir(parents=True, exist_ok=True)
        for source_file in (FIXTURE / name).iterdir():
            if source_file.is_file():
                shutil.copyfile(source_file, home / source_file.name)
    constants = "return {mark = " + json.dumps(str(runtime_mark())) + ", " + ", ".join(key + " = " + str(ids[key]) for key in ("larva", "pot")) + "}\n"
    (mods / "release.t3.night.observer/ids.lua").write_text(constants, encoding="utf-8")
    optional = output / "combat/romfs/tsuchi/mods/release.t3.night.combat"
    optional.mkdir(parents=True, exist_ok=True)
    for name in ("mod.json", "main.lua"):
        shutil.copyfile(FIXTURE / "release.t3.night.combat" / name, optional / name)
    (optional / "ids.lua").write_text(constants, encoding="utf-8")
    receipt = dict(package=PACKAGE, reference=reference, ids={"mark": str(runtime_mark()), "larva": str(ids["larva"]), "pot": str(ids["pot"])},
        template_mark=str(ids["mark"]), mark_note="Runtime replaces the donor template mark with the mod-and-package-bound hash",
        rows=rows, qualification=qualification, source_sha256={s:hashlib.sha256(donor.with_suffix(s).read_bytes()).hexdigest()
        for s in (".uasset", ".uexp")}, output_sha256={s:hashlib.sha256(relocated.with_suffix(s).read_bytes()).hexdigest()
        for s in (".uasset", ".uexp")}, reference_sha256=hashlib.sha256(reference_log.read_bytes()).hexdigest(),
        reference_path=str(reference_log), source_path=str(donor), natural_defeat=False, save_reload=False, cleanup=False)
    (output / "donor.json").write_text(json.dumps(receipt, indent=2)+"\n", encoding="utf-8")
    if pack:
        pack_stage(output / "stage", output / "out/romfs/Carrot4/Content/Paks", retoc)
    return receipt


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--old-tools", type=Path, required=True)
    parser.add_argument("--donor", type=Path, required=True)
    parser.add_argument("--reference-log", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--pack", action="store_true")
    parser.add_argument("--retoc", type=Path, help="retoc executable for --pack")
    args = parser.parse_args()
    print(json.dumps(stage(args.old_tools, args.donor, args.reference_log, args.output, args.pack, args.retoc), indent=2))
