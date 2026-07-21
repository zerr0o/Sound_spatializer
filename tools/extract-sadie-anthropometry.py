#!/usr/bin/env python3
"""Extract reproducible head dimensions from the public SADIE II 3D scans.

The full per-subject SADIE archives contain an OBJ scan in millimetres. This
tool streams only vertex records and writes the three primary head dimensions
used by the profile-selection k-medoids step. Archives are acquisition inputs;
only the small derived CSV belongs in the repository.
"""

from __future__ import annotations

import argparse
import csv
import math
import re
import zipfile
from pathlib import Path


SUBJECT_PATTERN = re.compile(r"H(?P<number>[3-9]|1[0-9]|20)\.zip$", re.IGNORECASE)


def scan_dimensions(archive: Path, subject: str) -> tuple[float, float, float, int]:
    expected_suffix = f"/{subject}_Scans/{subject}_3DScan.obj".lower()
    with zipfile.ZipFile(archive) as bundle:
        matches = [name for name in bundle.namelist() if name.lower().endswith(expected_suffix)]
        if len(matches) != 1:
            raise ValueError(f"{archive}: expected one {subject} OBJ scan, found {len(matches)}")
        minimum = [math.inf, math.inf, math.inf]
        maximum = [-math.inf, -math.inf, -math.inf]
        vertex_count = 0
        with bundle.open(matches[0]) as stream:
            for raw_line in stream:
                if not raw_line.startswith(b"v "):
                    continue
                fields = raw_line.split()
                if len(fields) < 4:
                    raise ValueError(f"{archive}: malformed OBJ vertex")
                values = [float(fields[index]) for index in range(1, 4)]
                if not all(math.isfinite(value) for value in values):
                    raise ValueError(f"{archive}: non-finite OBJ vertex")
                for axis, value in enumerate(values):
                    minimum[axis] = min(minimum[axis], value)
                    maximum[axis] = max(maximum[axis], value)
                vertex_count += 1
    if vertex_count < 1_000:
        raise ValueError(f"{archive}: implausibly small scan ({vertex_count} vertices)")
    spans = tuple(maximum[axis] - minimum[axis] for axis in range(3))
    if not all(100.0 <= span <= 400.0 for span in spans):
        raise ValueError(f"{archive}: implausible millimetre dimensions {spans}")
    # SADIE scans share X=left/right, Y=vertical, Z=back/front.
    return spans[0], spans[1], spans[2], vertex_count


def derive(archive_directory: Path) -> list[dict[str, str]]:
    archives: list[tuple[int, str, Path]] = []
    for archive in archive_directory.glob("H*.zip"):
        match = SUBJECT_PATTERN.fullmatch(archive.name)
        if match:
            number = int(match.group("number"))
            archives.append((number, f"H{number}", archive))
    if [number for number, _, _ in sorted(archives)] != list(range(3, 21)):
        raise ValueError("archive directory must contain exactly H3.zip through H20.zip")

    rows: list[dict[str, str]] = []
    for _, subject, archive in sorted(archives):
        width, height, depth, vertices = scan_dimensions(archive, subject)
        rows.append(
            {
                "subject": subject,
                "head_width_mm": f"{width:.6f}",
                "head_height_mm": f"{height:.6f}",
                "head_depth_mm": f"{depth:.6f}",
            }
        )
    return rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("archive_directory", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    rows = derive(args.archive_directory)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
