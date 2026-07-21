#!/usr/bin/env python3
"""Select morphologically diverse HRTF subjects with deterministic k-medoids.

The input CSV must contain a ``subject`` column and numeric anthropometric
features. Rows with incomplete features are rejected instead of silently
imputed. The output is a JSON array of subject identifiers. D2/KEMAR is added
separately by the product manifest and is not part of this human clustering.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import random
from pathlib import Path


def load_features(path: Path) -> tuple[list[str], list[list[float]]]:
    with path.open(newline="", encoding="utf-8-sig") as stream:
        rows = list(csv.DictReader(stream))
    if not rows or "subject" not in (rows[0].keys() if rows else []):
        raise ValueError("CSV must contain at least one row and a 'subject' column")

    feature_names = [name for name in rows[0] if name != "subject"]
    if not feature_names:
        raise ValueError("CSV must contain at least one numeric feature")

    subjects: list[str] = []
    values: list[list[float]] = []
    for line, row in enumerate(rows, start=2):
        subject = (row.get("subject") or "").strip()
        if not subject:
            raise ValueError(f"Missing subject on CSV line {line}")
        try:
            vector = [float(row[name]) for name in feature_names]
        except (TypeError, ValueError) as error:
            raise ValueError(f"Non-numeric or missing feature on CSV line {line}") from error
        if not all(math.isfinite(value) for value in vector):
            raise ValueError(f"Non-finite feature on CSV line {line}")
        subjects.append(subject)
        values.append(vector)

    return subjects, standardize(values)


def standardize(values: list[list[float]]) -> list[list[float]]:
    columns = list(zip(*values, strict=True))
    means = [sum(column) / len(column) for column in columns]
    scales = []
    for column, mean in zip(columns, means, strict=True):
        variance = sum((value - mean) ** 2 for value in column) / len(column)
        scales.append(math.sqrt(variance) or 1.0)
    return [
        [(value - mean) / scale for value, mean, scale in zip(row, means, scales, strict=True)]
        for row in values
    ]


def squared_distance(left: list[float], right: list[float]) -> float:
    return sum((a - b) ** 2 for a, b in zip(left, right, strict=True))


def assignment_cost(values: list[list[float]], medoids: tuple[int, ...]) -> float:
    return sum(min(squared_distance(value, values[index]) for index in medoids) for value in values)


def initialize_medoids(values: list[list[float]], count: int, seed: int) -> list[int]:
    if count < 1 or count > len(values):
        raise ValueError("count must be between 1 and the number of subjects")

    randomizer = random.Random(seed)
    indices = list(range(len(values)))

    # Seeded k-medoids++ initialization. The explicit PRNG makes the requested
    # seed meaningful while the following PAM swaps make the result locally
    # optimal and independent of incidental input iteration order.
    selected: list[int] = [randomizer.randrange(len(values))]
    while len(selected) < count:
        candidates = [index for index in indices if index not in selected]
        weights = [min(squared_distance(values[index], values[medoid]) for medoid in selected) for index in candidates]
        total = sum(weights)
        if total <= 0.0:
            selected.append(candidates[0])
            continue
        threshold = randomizer.random() * total
        cumulative = 0.0
        chosen = candidates[-1]
        for candidate, weight in zip(candidates, weights, strict=True):
            cumulative += weight
            if cumulative >= threshold:
                chosen = candidate
                break
        selected.append(chosen)
    return selected


def choose_medoids(values: list[list[float]], count: int, seed: int = 42) -> list[int]:
    selected = initialize_medoids(values, count, seed)
    indices = list(range(len(values)))

    # PAM SWAP: accept the best strictly improving medoid/non-medoid exchange.
    while True:
        baseline = assignment_cost(values, tuple(selected))
        best_cost = baseline
        best_swap: tuple[int, int] | None = None
        for position in range(len(selected)):
            for candidate in indices:
                if candidate in selected:
                    continue
                proposal = selected.copy()
                proposal[position] = candidate
                cost = assignment_cost(values, tuple(proposal))
                if cost < best_cost - 1e-12:
                    best_cost = cost
                    best_swap = (position, candidate)
        if best_swap is None:
            break
        selected[best_swap[0]] = best_swap[1]

    return sorted(selected)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", type=Path, help="Anthropometric CSV")
    parser.add_argument("--count", type=int, default=5)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    subjects, values = load_features(args.csv)
    selected = [subjects[index] for index in choose_medoids(values, args.count, args.seed)]
    payload = json.dumps(selected, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(payload, encoding="utf-8")
    else:
        print(payload, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
