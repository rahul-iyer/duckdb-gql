#!/usr/bin/env python3

import argparse
import csv
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "test" / "conformance" / "iso-gql-2024.tsv"
GRAMMAR = ROOT / "third_party" / "opengql" / "GQL.g4"
FEATURE_CORPUS = ROOT / "test" / "features" / "clauses"
EXPECTED_COLUMNS = ["feature_id", "area", "feature_family", "status", "test_scope"]
ALLOWED_STATUSES = {"planned", "partial", "implemented", "not_applicable"}
EXPECTED_GRAMMAR_RULES = 571
REQUIRED_SECTIONS = {f"GQL-{section:02d}" for section in range(6, 22)}


def parser_rule_count(grammar_text: str) -> int:
    return len(re.findall(r"(?m)^[a-z][A-Za-z0-9_]*\s*\n\s*:", grammar_text))


def section_covered(section: str, feature_ids: set[str]) -> bool:
    return any(feature_id == section or feature_id.startswith(section + ".") for feature_id in feature_ids)


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate the ISO GQL conformance source of record")
    parser.add_argument(
        "--release",
        action="store_true",
        help="also require every applicable feature family to be implemented",
    )
    args = parser.parse_args()

    with MANIFEST.open(newline="", encoding="utf-8") as manifest_file:
        reader = csv.DictReader(manifest_file, delimiter="\t")
        if reader.fieldnames != EXPECTED_COLUMNS:
            raise SystemExit(f"unexpected manifest columns: {reader.fieldnames}")
        rows = list(reader)

    if not rows:
        raise SystemExit("conformance manifest is empty")
    feature_ids = [row["feature_id"] for row in rows]
    duplicates = sorted(feature_id for feature_id, count in Counter(feature_ids).items() if count > 1)
    if duplicates:
        raise SystemExit(f"duplicate feature ids: {', '.join(duplicates)}")
    for line, row in enumerate(rows, start=2):
        empty_columns = [column for column in EXPECTED_COLUMNS if not row[column].strip()]
        if empty_columns:
            raise SystemExit(f"line {line}: empty columns: {', '.join(empty_columns)}")
        if row["status"] not in ALLOWED_STATUSES:
            raise SystemExit(f"line {line}: invalid status {row['status']!r}")

    feature_id_set = set(feature_ids)
    missing_sections = sorted(section for section in REQUIRED_SECTIONS if not section_covered(section, feature_id_set))
    if missing_sections:
        raise SystemExit(f"missing ISO grammar sections: {', '.join(missing_sections)}")

    actual_rule_count = parser_rule_count(GRAMMAR.read_text(encoding="utf-8"))
    if actual_rule_count != EXPECTED_GRAMMAR_RULES:
        raise SystemExit(
            f"OpenGQL grammar rule count changed: expected {EXPECTED_GRAMMAR_RULES}, found {actual_rule_count}; "
            "review the grammar delta and conformance manifest together"
        )

    subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "gql_fixture_adapter.py"), "--self-test"],
        cwd=ROOT,
        check=True,
    )
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "generate_gql_executable_candidates.py"),
            "--self-test",
        ],
        cwd=ROOT,
        check=True,
    )
    if FEATURE_CORPUS.is_dir():
        subprocess.run(
            [sys.executable, str(ROOT / "scripts" / "check_gql_feature_corpus.py")],
            cwd=ROOT,
            check=True,
        )
    else:
        print("skipped optional local GQL clause feature corpus (directory not present)")

    incomplete = [row["feature_id"] for row in rows if row["status"] not in {"implemented", "not_applicable"}]
    if args.release and incomplete:
        raise SystemExit(f"full-conformance release blocked by: {', '.join(incomplete)}")

    counts = Counter(row["status"] for row in rows)
    summary = ", ".join(f"{status}={counts[status]}" for status in sorted(counts))
    print(f"validated {len(rows)} ISO GQL feature families and {actual_rule_count} OpenGQL parser rules ({summary})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
