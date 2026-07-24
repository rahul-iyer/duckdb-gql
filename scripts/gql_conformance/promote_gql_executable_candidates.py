#!/usr/bin/env python3
"""Promote passing source-equivalent GQL candidates into the active suite."""

from __future__ import annotations

import argparse
import csv
import re
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
EXECUTION = ROOT / "test" / "features" / "execution.tsv"
CLAUSE_MANIFEST = ROOT / "test" / "features" / "clauses" / "manifest.tsv"
SQLLOGIC_ROOT = ROOT / "test" / "features" / "sqllogic"
SQLLOGIC_MANIFEST = SQLLOGIC_ROOT / "manifest.tsv"


def load_tsv(path: Path) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as source:
        return list(csv.DictReader(source, delimiter="\t"))


def candidate_body(path: Path) -> str:
    lines = path.read_text(encoding="utf-8").splitlines()
    result: list[str] = []
    for line in lines:
        if line.startswith("# name:") or line == "# group: [gql]":
            continue
        if line == "require duckgql":
            continue
        result.append(line)
    while result and not result[0]:
        result.pop(0)
    while result and not result[-1]:
        result.pop()
    return "\n".join(result)


def write_tsv(path: Path, rows: list[dict[str, str]], fieldnames: list[str]) -> None:
    with path.open("w", encoding="utf-8", newline="") as destination:
        writer = csv.DictWriter(
            destination, fieldnames=fieldnames, delimiter="\t", lineterminator="\n"
        )
        writer.writeheader()
        writer.writerows(rows)


def sync_sqllogic_metadata(execution_rows: list[dict[str, str]]) -> None:
    executions = {
        (row["source_path"], row["source_line"]): row for row in execution_rows
    }
    clause_rows = load_tsv(CLAUSE_MANIFEST)
    clauses_by_target: dict[str, list[dict[str, str]]] = {}
    clause_by_key: dict[tuple[str, str], dict[str, str]] = {}
    for row in clause_rows:
        clauses_by_target.setdefault(row["target_path"], []).append(row)
        clause_by_key[(row["source_path"], row["source_line"])] = row

    port_rows = load_tsv(SQLLOGIC_MANIFEST)
    port_by_target: dict[str, dict[str, str]] = {}
    source_prefix = "test/features/clauses/"
    for row in port_rows:
        target = row["source_feature"][len(source_prefix) :]
        port_by_target[target] = row
        scenarios = clauses_by_target[target]
        executable = sum(
            executions.get((scenario["source_path"], scenario["source_line"]), {}).get(
                "status"
            )
            == "executable"
            for scenario in scenarios
        )
        row["executable"] = str(executable)
        row["pending"] = str(len(scenarios) - executable)

    write_tsv(SQLLOGIC_MANIFEST, port_rows, list(port_rows[0]))

    ports: dict[Path, str] = {}
    for key, execution in executions.items():
        scenario = clause_by_key[key]
        port_row = port_by_target[scenario["target_path"]]
        port_path = ROOT / port_row["port_test"]
        contents = ports.get(port_path)
        if contents is None:
            contents = port_path.read_text(encoding="utf-8")
        marker = f"# Source: {execution['source_path']}:{execution['source_line']} "
        start = contents.find(marker)
        if start < 0:
            raise SystemExit(f"missing SQLLogic source marker {marker!r}")
        end = contents.find("\n# Source: tck/", start + len(marker))
        if end < 0:
            end = len(contents)
        block = contents[start:end]
        block = re.sub(
            r"(?m)^# Port status: .+$",
            f"# Port status: {execution['status']}",
            block,
            count=1,
        )
        block = re.sub(r"(?m)^# (?:Executable test|Review notes):.*\n?", "", block)
        if execution["status"] == "executable":
            lines = block.splitlines()
            insertion = next(
                (
                    index + 1
                    for index, line in enumerate(lines)
                    if line.startswith("# Translation review flags:")
                ),
                2,
            )
            lines[insertion:insertion] = [
                f"# Executable test: {execution['test_path']}",
                f"# Review notes: {execution['notes']}",
            ]
            block = "\n".join(lines) + ("\n" if block.endswith("\n") else "")
        contents = contents[:start] + block + contents[end:]
        ports[port_path] = contents

    for path, contents in ports.items():
        path.write_text(contents, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--candidates", type=Path)
    parser.add_argument("--runner", type=Path)
    parser.add_argument("--sync-only", action="store_true")
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / "test" / "sql" / "gql_feature_compatibility.test",
    )
    arguments = parser.parse_args()
    if arguments.sync_only:
        rows = load_tsv(EXECUTION)
        sync_sqllogic_metadata(rows)
        print(f"synchronized SQLLogic metadata for {len(rows)} execution rows")
        return 0
    if arguments.candidates is None or arguments.runner is None:
        parser.error("--candidates and --runner are required unless --sync-only is used")
    candidate_root = arguments.candidates.resolve()
    runner = arguments.runner.resolve()
    output = arguments.output.resolve()

    promoted: list[tuple[dict[str, str], Path]] = []
    for row in load_tsv(candidate_root / "manifest.tsv"):
        candidate = Path(row["test_path"])
        if not candidate.is_absolute():
            candidate = ROOT / candidate
        candidate = candidate.resolve()
        completed = subprocess.run(
            [str(runner), candidate.relative_to(ROOT).as_posix()],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        if completed.returncode == 0 and "All tests passed" in completed.stdout:
            promoted.append((row, candidate))

    relative_output = output.relative_to(ROOT).as_posix()
    contents = [
        f"# name: {relative_output}",
        "# description: Verified source-equivalent openCypher clause compatibility",
        "# group: [gql]",
        "#",
        "# GENERATED by scripts/gql_conformance/promote_gql_executable_candidates.py.",
        "# Each source-equivalent ISO-GQL adaptation passed independently before",
        "# inclusion here. Source licensing is retained under test/features/clauses.",
        "",
        "require duckgql",
        "",
    ]
    for _, candidate in promoted:
        contents.extend([candidate_body(candidate), ""])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(contents).rstrip() + "\n", encoding="utf-8")

    fieldnames = ["source_path", "source_line", "status", "test_path", "notes"]
    execution_rows = [
        row for row in load_tsv(EXECUTION) if row["test_path"] != relative_output
    ]
    existing = {
        (row["source_path"], row["source_line"]): row for row in execution_rows
    }
    for row, _ in promoted:
        key = (row["source_path"], row["source_line"])
        if key in existing:
            continue
        promoted_row = {
            "source_path": row["source_path"],
            "source_line": row["source_line"],
            "status": "executable",
            "test_path": relative_output,
            "notes": (
                "Source scenario translated to ISO GQL and verified "
                "against its adapted native-table fixture"
            ),
        }
        execution_rows.append(promoted_row)
        existing[key] = promoted_row
    execution_rows.sort(key=lambda row: (row["source_path"], int(row["source_line"])))
    write_tsv(EXECUTION, execution_rows, fieldnames)
    sync_sqllogic_metadata(execution_rows)

    print(
        f"promoted {len(promoted)} passing candidates to {relative_output}; "
        f"execution manifest now has {len(execution_rows)} rows"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
