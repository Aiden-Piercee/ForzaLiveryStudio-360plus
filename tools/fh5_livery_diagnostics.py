import argparse
import base64
import collections
import copy
import csv
import gzip
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import uuid
import zlib


IDENTITY = dict(x=0, y=0, scale_x=1, scale_y=1, rotation=0, skew=0)
SLOTS = ("Front", "Back", "Top", "Left", "Right", "Spoiler")


def walk(node):
    yield node
    for child in node.get("children", []):
        yield from walk(child)


def matrix(transform):
    t = IDENTITY | transform
    angle = math.radians(t["rotation"])
    c, s = math.cos(angle), math.sin(angle)
    return (c * t["scale_x"], (c * t["skew"] - s) * t["scale_y"], t["x"],
            s * t["scale_x"], (s * t["skew"] + c) * t["scale_y"], t["y"])


def multiply(a, b):
    return (a[0] * b[0] + a[1] * b[3], a[0] * b[1] + a[1] * b[4],
            a[0] * b[2] + a[1] * b[5] + a[2],
            a[3] * b[0] + a[4] * b[3], a[3] * b[1] + a[4] * b[4],
            a[3] * b[2] + a[4] * b[5] + a[5])


def decompose(m):
    sx = math.copysign(math.hypot(m[0], m[3]), m[0] * m[4] - m[1] * m[3])
    if abs(sx) < 1e-8:
        raise ValueError("Cannot flatten a singular transform")
    angle = math.atan2(m[3] / sx, m[0] / sx)
    c, s = math.cos(angle), math.sin(angle)
    sy = -s * m[1] + c * m[4]
    shear = c * m[1] + s * m[4]
    if abs(sy) <= 1e-8 and abs(shear) > 1e-8:
        raise ValueError("Cannot flatten a singular sheared transform")
    return dict(x=m[2], y=m[5], scale_x=sx, scale_y=sy,
                rotation=math.degrees(angle) % 360, skew=shear / sy if sy else 0)


def flatten(section):
    shapes = []

    def collect(node, parent, inherited_mask):
        world = multiply(parent, matrix(node.get("transform", {})))
        masked = inherited_mask or bool(node.get("debug", {}).get("flags", 0) & 64)
        if node.get("kind") == "shape":
            shape = copy.deepcopy(node)
            shape["transform"] = decompose(world)
            shape["mask"] = shape.get("mask", False) or masked
            shape.pop("debug", None)
            shapes.append(shape)
        for child in node.get("children", []):
            collect(child, world, masked)

    collect(section, matrix({}), False)
    section["transform"] = IDENTITY.copy()
    section["children"] = shapes
    section.pop("debug", None)


def prefix(node, remaining):
    if node.get("kind") == "shape":
        return remaining > 0, max(0, remaining - 1)
    children = []
    for child in node.get("children", []):
        keep, remaining = prefix(child, remaining)
        if keep:
            children.append(child)
    node["children"] = children
    return bool(children), remaining


def inflate(path):
    packed = path.read_bytes()
    raw = zlib.decompress(packed[8:])
    if struct.unpack_from("<II", packed) != (len(packed) - 8, len(raw)):
        raise ValueError(f"Invalid container lengths: {path}")
    artwork = raw.index(b"gyvl")
    size = struct.unpack_from("<I", raw, artwork - 4)[0]
    return raw, artwork, raw[artwork + 21:artwork + size]


def replace_body(path, body):
    raw, artwork, old = inflate(path)
    raw = bytearray(raw[:artwork + 21] + body + raw[artwork + 21 + len(old):])
    struct.pack_into("<I", raw, artwork - 4, len(body) + 21)
    packed = zlib.compress(raw)
    path.write_bytes(struct.pack("<II", len(packed), len(raw)) + packed)


def run(tool, arguments, log, dump=False):
    env = os.environ | {"QT_FORCE_STDERR_LOGGING": "1"}
    if dump:
        env["FLS_DUMP_RECORDS"] = "1"
    result = subprocess.run([str(tool), *map(str, arguments)], env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = result.stdout.decode("utf-8", errors="replace")
    log.write_text(text, encoding="utf-8")
    return result.returncode, text


def inspect(tool, path, log):
    code, text = run(tool, ["--inspect", path], log, dump=True)
    if code:
        raise ValueError(text)
    records = [(int(slot), int(offset), int(identifier)) for slot, offset, identifier
               in re.findall(r"RECORD (\d+) (\d+) (\d+)", text)]
    sections = [(name, int(offset)) for name, offset in
                re.findall(r'^"(\w+)" offset (\d+)', text, re.MULTILINE)]
    return records, sections


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("project", type=Path)
    parser.add_argument("original", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--tool", type=Path, default=Path("build/Release/fls_game_export_tests.exe"))
    args = parser.parse_args()
    tool = args.tool.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    audit = output / "audit"
    audit.mkdir()
    projects = output / "projects"
    projects.mkdir()
    source = json.loads(gzip.decompress(args.project.read_bytes()))
    source["root"]["children"] = [n for n in source["root"]["children"]
        if n.get("is_livery_section") and 0 <= n["livery_section_slot"] < 6]
    original_raw, _, original_body = inflate(args.original)
    original_records, original_sections = inspect(tool, args.original, audit / "original.log")
    original_records = [record for record in original_records if record[0] < 6]
    source_counts = [sum(n.get("kind") == "shape" for n in walk(section))
                     for section in source["root"]["children"]]
    if collections.Counter(r[0] for r in original_records) != collections.Counter(
            {s["livery_section_slot"]: c for s, c in zip(source["root"]["children"], source_counts) if c}):
        raise ValueError("Original artwork and project section counts differ")
    results = []

    def export_case(number, title, description, *, slots=range(6), masks=True,
                    flat=False, markers=False, limit=None, clear_source=False):
        label = f"{number:02d}_{title}"
        document = copy.deepcopy(source)
        document["name"] = f"D{number:02d} {title}"[:32]
        document["header_metadata"]["guid"] = base64.b64encode(uuid.uuid4().bytes_le).decode()
        for section in document["root"]["children"]:
            slot = section["livery_section_slot"]
            if slot not in slots:
                section["children"] = []
            elif markers:
                template = copy.deepcopy(next(n for n in walk(section) if n.get("kind") == "shape"))
                template["visual"] = {"kind": "vector", "shape_id": 101}
                template["transform"] = IDENTITY | dict(scale_x=0.5, scale_y=0.5)
                template["color"] = [[255, 0, 0, 255], [0, 255, 0, 255], [0, 0, 255, 255],
                                     [255, 255, 0, 255], [255, 0, 255, 255], [0, 255, 255, 255]][slot]
                template["mask"] = False
                template.pop("debug", None)
                section["children"] = [template]
                section["transform"] = IDENTITY.copy()
            elif limit is not None and slot == 1:
                prefix(section, limit)
            if flat:
                flatten(section)
        for node in walk(document["root"]):
            if not masks:
                if node.get("kind") == "shape":
                    node["mask"] = False
                if "debug" in node:
                    node["debug"]["flags"] &= ~64
            if clear_source:
                node.pop("debug", None)
        if clear_source or flat or markers:
            document["livery_source"] = ""
        project_path = projects / (label + ".3so")
        project_path.write_bytes(gzip.compress(json.dumps(document).encode(), mtime=0))
        folder = output / label
        code, check_text = run(tool, ["--export-fh5", project_path, folder], audit / (label + ".log"))
        if not (folder / "C_livery").exists():
            raise ValueError(f"Export failed: {label}; see audit log")
        counts = [sum(n.get("kind") == "shape" for n in walk(section))
                  for section in document["root"]["children"]]
        row = dict(id=f"D{number:02d}", folder=label, purpose=description, counts=counts,
                   masked_shapes=sum(bool(n.get("mask")) for n in walk(document["root"])),
                   artwork_check="PASS" if code == 0 else "FAIL (see audit log)",
                   check_error=check_text.strip() if code else "",
                   game_result="", first_bad_section="", notes="")
        results.append(row)
        print(label, row["artwork_check"], counts, flush=True)
        return folder, project_path, row

    export_case(0, "six_markers", "Six plain squares: Front red, Back green, Top blue, Left yellow, Right magenta, Spoiler cyan", markers=True)
    baseline, _, _ = export_case(1, "full_current", "Unmodified encoder baseline; all body artwork")
    _, _, baseline_body = inflate(baseline / "C_livery")
    baseline_records, _ = inspect(tool, baseline / "C_livery", audit / "baseline-records.log")
    if len(baseline_records) != len(original_records):
        raise ValueError("Baseline and original record counts differ")
    removals = []
    forced_leads = []
    for index, (before, after) in enumerate(zip(original_records, baseline_records)):
        if before[0] != after[0] or before[2] != after[2]:
            raise ValueError(f"Record identity differs at {index}")
        a, b = before[1], after[1]
        if original_body[a] == 2 and baseline_body[b] in (0, 1) and baseline_body[b + 1] == 2:
            forced_leads.append((b, b + 1, index))
        if index == 0:
            continue
        pa, pb = original_records[index - 1][1], baseline_records[index - 1][1]
        a_end = pa + (1 if original_body[pa] == 2 else 2) + 30
        b_end = pb + (1 if baseline_body[pb] == 2 else 2) + 30
        gap = baseline_body[b_end:b]
        if a_end == a and gap and gap[0] == 0 and all(v == 1 for v in gap[1:]):
            removals.append((b_end, b, index))
    if not removals:
        raise ValueError("No extra group-closing bytes found")
    first_index = removals[0][2]
    first_slot = baseline_records[first_index][0]
    if first_slot != 1:
        raise ValueError("Prefix controls require the first boundary in Back")
    prefix_count = sum(r[0] == first_slot for r in baseline_records[:first_index])
    audit_data = dict(original=str(args.original.resolve()), project=str(args.project.resolve()),
                      original_raw_bytes=len(original_raw), original_sections=original_sections,
                      first_shift_record=first_index, first_back_prefix=prefix_count,
                      extra_group_closing_ranges=removals, forced_shape_lead_ranges=forced_leads)
    (audit / "boundaries.json").write_text(json.dumps(audit_data, indent=2), encoding="utf-8")
    export_case(2, "full_no_masks", "Full hierarchy, mask flags cleared", masks=False)
    export_case(3, "full_flat", "All shapes flattened within each section; masks retained", flat=True)
    export_case(4, "full_flat_no_masks", "Flat shapes and no masks", flat=True, masks=False)
    for slot, name in enumerate(SLOTS):
        export_case(5 + slot, "only_" + name.lower(), f"Only {name}; other five body slots empty", slots=[slot])
    for end in range(2, 6):
        export_case(9 + end, f"first_{end}_sections", f"Original artwork from Front through {SLOTS[end - 1]}", slots=range(end))
    export_case(15, "before_first_gap", f"Front plus first {prefix_count} Back shapes, before first extra group-closing byte", slots=[0, 1], limit=prefix_count)
    export_case(16, "after_first_gap", f"Front plus first {prefix_count + 1} Back shapes, crossing first extra group-closing byte", slots=[0, 1], limit=prefix_count + 1)

    def patched_case(number, title, description, ranges=None, native=False, masks=True):
        folder, project, row = export_case(number, title, description, masks=masks)
        if native:
            end = next(offset for name, offset in original_sections if name == "FrontWindshield")
            body = original_body[:end]
        else:
            _, _, body = inflate(folder / "C_livery")
            if len(body) != len(baseline_body):
                raise ValueError("Experimental patches require matching record offsets")
            for start, end, _ in sorted(ranges, reverse=True):
                body = body[:start] + body[end:]
        replace_body(folder / "C_livery", body)
        code, check_text = run(tool, ["--compare-livery", project, folder], audit / (row["folder"] + ".patched.log"))
        row["artwork_check"] = "PASS" if code == 0 else "FAIL (see patched audit log)"
        row["check_error"] = check_text.strip() if code else ""
        row["artwork_byte_delta"] = len(body) - len(baseline_body)
        row["experimental"] = True
        print("Patched", row["folder"], row["artwork_check"], row["artwork_byte_delta"], flush=True)

    patched_case(17, "remove_group_closers", "Full livery; remove only extra group-closing bytes", removals)
    patched_case(18, "restore_bare_leads", "D17 plus restore the original bare shape leads", removals + forced_leads)
    patched_case(19, "original_body_artwork", "FH5 metadata envelope with original FH6 body-section bytes; windows omitted", native=True)
    export_case(20, "full_fresh_frames", "Full hierarchy rebuilt without imported framing metadata", clear_source=True)
    patched_case(21, "closers_no_masks", "D17 with all mask flags cleared", removals, masks=False)
    patched_case(22, "bare_leads_no_masks", "D18 with all mask flags cleared", removals + forced_leads, masks=False)
    (output / "manifest.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    with (output / "results.csv").open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=["id", "folder", "purpose", "counts", "artwork_check", "check_error", "game_result", "first_bad_section", "notes"], extrasaction="ignore")
        writer.writeheader()
        writer.writerows(results)
    (output / "TEST_PLAN.txt").write_text(
        "FH5 in-game diagnostic liveries, car ID " + str(source["car_id"]) + "\n\n"
        "Load each numbered asset folder as a PGS livery. Inspect the applied artwork on the car.\n"
        "Record PASS (all intended artwork correct), CORRUPT (loads with garbage/missing artwork),\n"
        "or REJECTED (does not load) in results.csv, and the first affected section.\n"
        "Prioritize D00, D01, D05, D06, D11, D15, D16, D17, D18, D19, D21, D22.\n"
        "Then D02-D04 distinguish masks and grouping; D07-D10 isolate remaining sections;\n"
        "D12-D14 locate a later multi-section boundary; D20 isolates retained source framing.\n"
        "D21-D22 remove masks from the two framing experiments.\n"
        "D17-D19 and D21-D22 are byte-level experiments, not verified in-game fixes.\n"
        "Known mask mismatches are recorded in check_error; these are intentional diagnostic controls.\n"
        "Automated artwork checks use FLS's decoder; their PASS does not establish FH5 readability.\n"
        "All variants have unique header IDs and Dxx names. Source files are untouched.\n"
        "The marker control uses one centered colored square per body section.\n",
        encoding="utf-8")
    print("Prepared", len(results), "liveries in", output, flush=True)


if __name__ == "__main__":
    main()
