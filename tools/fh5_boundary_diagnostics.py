import argparse
import base64
import copy
import csv
import gzip
import json
from pathlib import Path
import struct
import uuid

from fh5_livery_diagnostics import (
    IDENTITY, flatten, inflate, inspect, replace_body, run, walk,
)


def payload(folder):
    files = [p for p in folder.iterdir() if p.name == "C_livery"
             or p.suffix.lower() == ".c_livery"]
    if len(files) != 1:
        raise ValueError(f"Expected one livery payload in {folder}")
    return files[0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("diagnostics", type=Path)
    parser.add_argument("original", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--tool", type=Path, default=Path("build/Release/fls_game_export_tests.exe"))
    args = parser.parse_args()
    root, output, tool = args.diagnostics.resolve(), args.output.resolve(), args.tool.resolve()
    output.mkdir(parents=True, exist_ok=False)
    audit = output / "audit"
    audit.mkdir()
    projects = output / "projects"
    projects.mkdir()
    source_path = root / "projects/11_first_2_sections.3so"
    source = json.loads(gzip.decompress(source_path.read_bytes()))
    baseline_path = payload(root / "11_first_2_sections")
    _, _, baseline = inflate(baseline_path)
    baseline_records, baseline_sections = inspect(tool, baseline_path, audit / "D11-records.log")
    _, _, original = inflate(args.original)
    original_records, original_sections = inspect(tool, args.original, audit / "original-records.log")
    original_records = [r for r in original_records if r[0] in (0, 1)]
    if [(r[0], r[2]) for r in original_records] != [(r[0], r[2]) for r in baseline_records]:
        raise ValueError("Original and baseline artwork records differ")
    front_back = dict(baseline_sections)["Back"]
    front_last = next(r for r in reversed(baseline_records) if r[0] == 0)[1]
    front_end = front_last + (1 if baseline[front_last] == 2 else 2) + 30
    separator = baseline[front_end:front_back]
    if separator != bytes(9) + struct.pack("<ff", 1, 0) + bytes(1):
        raise ValueError("Unexpected Front/Back separator")
    ranges = []
    for index in range(1, len(original_records)):
        previous_source = original_records[index - 1][1]
        previous_rebuilt = baseline_records[index - 1][1]
        source_end = previous_source + (1 if original[previous_source] == 2 else 2) + 30
        rebuilt_end = previous_rebuilt + (1 if baseline[previous_rebuilt] == 2 else 2) + 30
        gap = baseline[rebuilt_end:baseline_records[index][1]]
        if source_end == original_records[index][1] and gap and gap[0] == 0 and all(v == 1 for v in gap[1:]):
            ranges.append((rebuilt_end, baseline_records[index][1]))
    results = []

    def make_case(number, title, description, mode="source", edit=None):
        label = f"{number:02d}_{title}"
        document = copy.deepcopy(source)
        document["name"] = f"D{number:02d} {title}"[:32]
        document["header_metadata"]["guid"] = base64.b64encode(uuid.uuid4().bytes_le).decode()
        back = next(s for s in document["root"]["children"] if s["livery_section_slot"] == 1)
        if mode == "flat":
            flatten(back)
        elif mode in ("two-flat", "two-grouped"):
            template = copy.deepcopy(next(n for n in walk(back) if n.get("kind") == "shape"))
            shapes = []
            for i, color in enumerate(([0, 255, 0, 255], [0, 128, 255, 255])):
                shape = copy.deepcopy(template)
                shape.update(id=f"diagnostic-back-{i}", name=f"Back square {i + 1}",
                             visual={"kind": "vector", "shape_id": 101}, color=color,
                             transform=IDENTITY | dict(x=-15 + i * 30, scale_x=0.5, scale_y=0.5),
                             mask=False)
                shape.pop("debug", None)
                shapes.append(shape)
            back["transform"] = IDENTITY.copy()
            back["children"] = shapes if mode == "two-flat" else [dict(
                kind="group", id="diagnostic-back-group", name="Two Back squares",
                transform=IDENTITY.copy(), visible=True, opacity=1, children=shapes)]
        if mode != "source":
            document["livery_source"] = ""
            for node in walk(document["root"]):
                node.pop("debug", None)
        project = projects / (label + ".3so")
        project.write_bytes(gzip.compress(json.dumps(document).encode(), mtime=0))
        folder = output / label
        code, log = run(tool, ["--export-fh5", project, folder], audit / (label + ".export.log"))
        if code:
            raise ValueError(f"Export artwork check failed for {label}: {log}")
        file = payload(folder)
        _, _, before = inflate(file)
        after = edit(before) if edit else before
        if edit:
            replace_body(file, after)
            code, log = run(tool, ["--compare-livery", project, folder], audit / (label + ".patched.log"))
        counts = [sum(n.get("kind") == "shape" for n in walk(s)) for s in document["root"]["children"]]
        result = dict(id=f"D{number:02d}", folder=label, purpose=description,
                      counts=counts, artwork_check="PASS" if code == 0 else "FAIL",
                      artwork_byte_delta=len(after) - len(before), game_result="",
                      first_bad_section="", notes="", check_error=log.strip() if code else "")
        results.append(result)
        inspect(tool, file, audit / (label + ".records.log"))
        if baseline_path.name != "C_livery":
            file.rename(file.with_name(baseline_path.name))
        print(result["id"], label, result["artwork_check"], counts,
              "byte delta", result["artwork_byte_delta"], flush=True)

    def remove_closers(body):
        if body != baseline:
            raise ValueError("D11 rebuild changed before the byte-only experiment")
        for start, end in reversed(ranges):
            body = body[:start] + body[end:]
        return body

    def native_back(body):
        if body != baseline:
            raise ValueError("D11 rebuild changed before the native artwork experiment")
        source_end = dict(original_sections)["Top"]
        rebuilt_end = dict(baseline_sections)["Top"]
        return original[:source_end] + baseline[rebuilt_end:]

    def first_group_marker(body):
        back = front_back
        children, blocks = struct.unpack_from("<HH", body, back)
        if children != 1 or blocks != 1 or body[back + 6] != 1:
            raise ValueError("Expected one grouped Back child")
        transform = back + 6 + blocks
        if body[transform + 16] != 32:
            raise ValueError("Expected a counted group after the unmarked transform")
        return body[:transform] + b"\x01" + body[transform:]

    def markerless_first_group(body):
        back = front_back
        group = back + 7 + 16
        if body[back:back + 7] != bytes.fromhex("01000100000001") or body[group] != 32:
            raise ValueError("Expected the grouped Back control")
        return body[:group] + body[group + 1:]

    make_case(23, "D11_remove_closers", "D11 with only the ten extra Back group-closing bytes removed", edit=remove_closers)
    make_case(24, "D11_native_back", "D11 with the original Front and Back artwork bytes, retaining its FH5 envelope and empty slots", edit=native_back)
    make_case(25, "D11_flat_back", "Original Front and 206 Back shapes, flattened within Back", mode="flat")
    make_case(26, "front_back_two_flat", "Original Front and two ungrouped Back squares", mode="two-flat")
    make_case(27, "front_back_two_grouped", "D26 with the two Back squares grouped", mode="two-grouped")
    make_case(28, "group_transform_lead", "D27 with explicit 01 before the first Back group transform", mode="two-grouped", edit=first_group_marker)
    make_case(29, "markerless_first_group", "D27 with a markerless first Back artwork group", mode="two-grouped", edit=markerless_first_group)
    (output / "manifest.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    with (output / "results.csv").open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(results[0]))
        writer.writeheader()
        writer.writerows(results)
    report = dict(user_results={
        "D00": "PASS", "D05": "PASS", "D06": "UNTESTED",
        "D11": dict(result="CORRUPT", observed_in_reported_order=[
            ["Front", 6, "correct"], ["Left", 30, "garbage"], ["Top", 40, "garbage"],
            ["Right", 40, "garbage"], ["Back", 64010, "garbage"], ["Back", 40, "garbage"]]),
    }, d11_front_back_separator=separator.hex(), d11_removed_byte_ranges=ranges,
                  removed_bytes=sum(end - start for start, end in ranges),
                  production_encoder_changed=False)
    (audit / "investigation.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    (output / "TEST_PLAN.txt").write_text(
        "FH5 second diagnostic batch: D11 boundary\n\n"
        "Car ID 368. Original Front contains six shapes in every case.\n"
        "Test D23, D24, D25 first. Each should have Front=6, Back=206, all other sections empty.\n"
        "D23 removes only ten Back group-closing bytes. D24 retains original artwork framing.\n"
        "D25 retains shape appearance and flattens Back groups.\n"
        "Then D26 and D27 should both have Front=6, Back=2, all other sections empty.\n"
        "Back has a green and a blue square. D26 is flat; D27 groups them.\n"
        "D28 and D29 vary only the first group framing from D27.\n"
        "Record PASS, CORRUPT, or REJECTED in results.csv with displayed section counts.\n"
        "All PASS entries in artwork_check refer to the automated FLS comparison only.\n"
        "Existing D00-D22 files are untouched. New payloads use D11's current payload filename.\n",
        encoding="utf-8")
    if any(row["artwork_check"] != "PASS" for row in results):
        raise ValueError("One or more diagnostic artwork comparisons failed; see audit logs")


if __name__ == "__main__":
    main()
