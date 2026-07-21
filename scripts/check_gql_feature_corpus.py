#!/usr/bin/env python3
"""Validate the source pin, coverage, and mechanical GQL clause rewrites."""

from __future__ import annotations

import csv
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CORPUS = ROOT / "test" / "features" / "clauses"
SQLLOGIC_ROOT = ROOT / "test" / "features" / "sqllogic"
EXPECTED_FILES = 93
EXPECTED_SCENARIOS = 827
EXPECTED_FIXTURE_SETUPS = 548
EXPECTED_REVISION = "677cbafabb8c3c5eed458fd3b1ec0daec8d67d23"
NOTICE = "# Modified by duckdb-gql:"


def query_blocks(text: str, path: Path) -> list[str]:
    blocks: list[str] = []
    current: list[str] | None = None
    for line in text.splitlines():
        if line.strip() == '"""':
            if current is None:
                current = []
            else:
                blocks.append("\n".join(current))
                current = None
        elif current is not None:
            current.append(line)
    if current is not None:
        raise SystemExit(f"unterminated query doc string: {path}")
    return blocks


def main() -> int:
    metadata = json.loads((CORPUS / "SOURCE.json").read_text(encoding="utf-8"))
    if metadata["revision"] != EXPECTED_REVISION:
        raise SystemExit(f"unexpected openCypher revision: {metadata['revision']}")

    features = sorted(CORPUS.rglob("*.feature"))
    if len(features) != EXPECTED_FILES:
        raise SystemExit(f"expected {EXPECTED_FILES} feature files, found {len(features)}")
    declarations = 0
    for feature in features:
        text = feature.read_text(encoding="utf-8")
        if NOTICE not in text:
            raise SystemExit(f"missing modification notice: {feature}")
        declarations += len(re.findall(r"(?m)^\s*Scenario(?: Outline)?:", text))
        for block in query_blocks(text, feature):
            if re.search(r"(?im)^\s*UNWIND\b", block):
                raise SystemExit(f"untranslated UNWIND in {feature}")
            if re.search(r"(?im)^\s*WITH\b", block):
                raise SystemExit(f"untranslated Cypher WITH in {feature}")
            if re.search(r"(?i)\bSKIP\b", block):
                raise SystemExit(f"untranslated SKIP in {feature}")
            for line in block.splitlines():
                without_actions = re.sub(r"(?i)\bON\s+CREATE\b", "", line)
                if re.search(r"(?i)\bCREATE\b", without_actions):
                    raise SystemExit(f"untranslated CREATE in {feature}")
            if re.search(r"(?i)\bid\s*\(", block):
                raise SystemExit(f"untranslated id() in {feature}")
    if declarations != EXPECTED_SCENARIOS:
        raise SystemExit(
            f"expected {EXPECTED_SCENARIOS} scenario declarations, found {declarations}"
        )

    with (CORPUS / "manifest.tsv").open(encoding="utf-8", newline="") as source:
        rows = list(csv.DictReader(source, delimiter="\t"))
    if len(rows) != EXPECTED_SCENARIOS:
        raise SystemExit(f"manifest contains {len(rows)} rows, expected {EXPECTED_SCENARIOS}")
    allowed_statuses = {"ported_unverified", "executable", "unsupported"}
    scenario_keys: set[tuple[str, str]] = set()
    manifest_by_target: dict[str, list[dict[str, str]]] = {}
    for line, row in enumerate(rows, start=2):
        if row["status"] not in allowed_statuses:
            raise SystemExit(f"manifest line {line}: invalid status {row['status']!r}")
        if not (CORPUS / row["target_path"]).is_file():
            raise SystemExit(f"manifest line {line}: missing target {row['target_path']}")
        key = (row["source_path"], row["source_line"])
        if key in scenario_keys:
            raise SystemExit(f"manifest line {line}: duplicate scenario key {key}")
        scenario_keys.add(key)
        manifest_by_target.setdefault(row["target_path"], []).append(row)

    execution_path = ROOT / "test" / "features" / "execution.tsv"
    with execution_path.open(encoding="utf-8", newline="") as source:
        execution_rows = list(csv.DictReader(source, delimiter="\t"))
    executions = {
        (row["source_path"], row["source_line"]): row for row in execution_rows
    }
    execution_keys: set[tuple[str, str]] = set()
    executable_count = 0
    unsupported_count = 0
    test_text: dict[Path, str] = {}
    for line, row in enumerate(execution_rows, start=2):
        key = (row["source_path"], row["source_line"])
        if key not in scenario_keys:
            raise SystemExit(f"execution line {line}: unknown scenario key {key}")
        if key in execution_keys:
            raise SystemExit(f"execution line {line}: duplicate scenario key {key}")
        execution_keys.add(key)
        if row["status"] not in {"executable", "unsupported"}:
            raise SystemExit(f"execution line {line}: invalid status {row['status']!r}")
        if row["status"] == "executable":
            test_path = ROOT / row["test_path"]
            if not row["test_path"] or not test_path.is_file():
                raise SystemExit(f"execution line {line}: missing executable test artifact")
            source_marker = f"# Source: {row['source_path']}:{row['source_line']}"
            contents = test_text.setdefault(
                test_path, test_path.read_text(encoding="utf-8")
            )
            if source_marker not in contents:
                raise SystemExit(
                    f"execution line {line}: test artifact is missing {source_marker!r}"
                )
            executable_count += 1
        elif not row["notes"].strip():
            raise SystemExit(f"execution line {line}: unsupported status requires notes")
        else:
            unsupported_count += 1

    with (SQLLOGIC_ROOT / "manifest.tsv").open(
        encoding="utf-8", newline=""
    ) as source:
        port_rows = list(csv.DictReader(source, delimiter="\t"))
    with (SQLLOGIC_ROOT / "fixtures.tsv").open(
        encoding="utf-8", newline=""
    ) as source:
        fixture_rows = list(csv.DictReader(source, delimiter="\t"))
    if len(fixture_rows) != EXPECTED_FIXTURE_SETUPS:
        raise SystemExit(
            f"fixture manifest contains {len(fixture_rows)} scenario setups, "
            f"expected {EXPECTED_FIXTURE_SETUPS}"
        )
    fixtures: dict[tuple[str, str], dict[str, str]] = {}
    fixture_rows_by_port: dict[str, list[dict[str, str]]] = {}
    fixture_ready_count = 0
    for line, fixture_row in enumerate(fixture_rows, start=2):
        key = (fixture_row["source_path"], fixture_row["source_line"])
        if key not in scenario_keys:
            raise SystemExit(f"fixture manifest line {line}: unknown scenario {key}")
        if key in fixtures:
            raise SystemExit(f"fixture manifest line {line}: duplicate scenario {key}")
        fixtures[key] = fixture_row
        fixture_rows_by_port.setdefault(fixture_row["port_test"], []).append(
            fixture_row
        )
        if fixture_row["status"] not in {"ready", "pending"}:
            raise SystemExit(
                f"fixture manifest line {line}: invalid status {fixture_row['status']!r}"
            )
        try:
            vertices = int(fixture_row["vertices"])
            edges = int(fixture_row["edges"])
        except ValueError as error:
            raise SystemExit(
                f"fixture manifest line {line}: counts must be integers"
            ) from error
        if vertices < 0 or edges < 0:
            raise SystemExit(f"fixture manifest line {line}: negative element count")
        if fixture_row["status"] == "ready":
            if fixture_row["reason"]:
                raise SystemExit(
                    f"fixture manifest line {line}: ready fixture has a failure reason"
                )
            fixture_ready_count += 1
        elif not fixture_row["reason"]:
            raise SystemExit(
                f"fixture manifest line {line}: pending fixture requires a reason"
            )
    if len(port_rows) != EXPECTED_FILES:
        raise SystemExit(
            f"SQLLogic manifest contains {len(port_rows)} files, expected {EXPECTED_FILES}"
        )
    expected_port_paths: set[Path] = set()
    port_scenario_count = 0
    for line, port_row in enumerate(port_rows, start=2):
        source_prefix = "test/features/clauses/"
        source_feature = port_row["source_feature"]
        if not source_feature.startswith(source_prefix):
            raise SystemExit(f"SQLLogic manifest line {line}: invalid source path")
        target_path = source_feature[len(source_prefix) :]
        if target_path not in manifest_by_target:
            raise SystemExit(
                f"SQLLogic manifest line {line}: unknown source feature {source_feature}"
            )
        port_test = ROOT / port_row["port_test"]
        if port_test in expected_port_paths:
            raise SystemExit(
                f"SQLLogic manifest line {line}: duplicate port path {port_row['port_test']}"
            )
        expected_port_paths.add(port_test)
        if not port_test.is_file():
            raise SystemExit(
                f"SQLLogic manifest line {line}: missing port {port_row['port_test']}"
            )
        scenario_rows = manifest_by_target[target_path]
        try:
            scenario_count = int(port_row["scenarios"])
            port_executable = int(port_row["executable"])
            port_pending = int(port_row["pending"])
            port_fixture_setups = int(port_row["fixture_setups"])
            port_fixture_ready = int(port_row["fixture_ready"])
            port_fixture_pending = int(port_row["fixture_pending"])
        except ValueError as error:
            raise SystemExit(
                f"SQLLogic manifest line {line}: counts must be integers"
            ) from error
        if scenario_count != len(scenario_rows):
            raise SystemExit(
                f"SQLLogic manifest line {line}: scenario count mismatch"
            )
        expected_executable = sum(
            executions.get((row["source_path"], row["source_line"]), {}).get(
                "status"
            )
            == "executable"
            for row in scenario_rows
        )
        if port_executable != expected_executable:
            raise SystemExit(
                f"SQLLogic manifest line {line}: executable count mismatch"
            )
        if port_pending != scenario_count - expected_executable:
            raise SystemExit(f"SQLLogic manifest line {line}: pending count mismatch")
        port_fixtures = fixture_rows_by_port.get(port_row["port_test"], [])
        expected_fixture_ready = sum(
            fixture["status"] == "ready" for fixture in port_fixtures
        )
        if port_fixture_setups != len(port_fixtures):
            raise SystemExit(
                f"SQLLogic manifest line {line}: fixture setup count mismatch"
            )
        if port_fixture_ready != expected_fixture_ready:
            raise SystemExit(
                f"SQLLogic manifest line {line}: fixture ready count mismatch"
            )
        if port_fixture_pending != len(port_fixtures) - expected_fixture_ready:
            raise SystemExit(
                f"SQLLogic manifest line {line}: fixture pending count mismatch"
            )
        contents = port_test.read_text(encoding="utf-8")
        for scenario_row in scenario_rows:
            marker = (
                f"# Source: {scenario_row['source_path']}:"
                f"{scenario_row['source_line']} {scenario_row['scenario']}"
            )
            if marker not in contents:
                raise SystemExit(
                    f"SQLLogic port {port_row['port_test']}: missing {marker!r}"
                )
            execution = executions.get(
                (scenario_row["source_path"], scenario_row["source_line"])
            )
            expected_status = execution["status"] if execution else "pending"
            status_marker = f"# Port status: {expected_status}"
            marker_position = contents.index(marker)
            next_marker = contents.find("\n# Source: tck/", marker_position + 1)
            scenario_port = contents[
                marker_position : next_marker if next_marker >= 0 else None
            ]
            if status_marker not in scenario_port:
                raise SystemExit(
                    f"SQLLogic port {port_row['port_test']}: missing {status_marker!r}"
                )
            if execution and execution["status"] == "executable":
                executable_marker = f"# Executable test: {execution['test_path']}"
                if executable_marker not in scenario_port:
                    raise SystemExit(
                        f"SQLLogic port {port_row['port_test']}: missing "
                        f"{executable_marker!r}"
                    )
            fixture = fixtures.get(
                (scenario_row["source_path"], scenario_row["source_line"])
            )
            if fixture:
                adaptation_marker = (
                    f"# INSERT fixture adaptation: {fixture['status']}"
                )
                if adaptation_marker not in scenario_port:
                    raise SystemExit(
                        f"SQLLogic port {port_row['port_test']}: missing "
                        f"{adaptation_marker!r}"
                    )
                if fixture["status"] == "ready":
                    if "# SQLLogic fixture setup:" not in scenario_port:
                        raise SystemExit(
                            f"SQLLogic port {port_row['port_test']}: ready fixture "
                            "has no SQLLogic setup"
                        )
                    if "# CREATE GRAPH " not in scenario_port:
                        raise SystemExit(
                            f"SQLLogic port {port_row['port_test']}: ready fixture "
                            "has no CREATE GRAPH command"
                        )
            elif "# INSERT fixture adaptation:" in scenario_port:
                raise SystemExit(
                    f"SQLLogic port {port_row['port_test']}: unexpected fixture marker"
                )
        port_scenario_count += scenario_count
    actual_port_paths = set((SQLLOGIC_ROOT / "clauses").rglob("*.test"))
    if actual_port_paths != expected_port_paths:
        extra = sorted(str(path.relative_to(ROOT)) for path in actual_port_paths - expected_port_paths)
        missing = sorted(str(path.relative_to(ROOT)) for path in expected_port_paths - actual_port_paths)
        raise SystemExit(f"SQLLogic port file set mismatch: extra={extra}, missing={missing}")
    if port_scenario_count != EXPECTED_SCENARIOS:
        raise SystemExit(
            f"SQLLogic ports contain {port_scenario_count} scenarios, expected {EXPECTED_SCENARIOS}"
        )
    print(
        f"validated {len(features)} GQL clause feature files and "
        f"{declarations} source-mapped scenario declarations "
        f"(executable={executable_count}, unsupported={unsupported_count}, "
        f"unreviewed={declarations - len(execution_rows)}); "
        f"validated {len(port_rows)} SQLLogic port files and "
        f"{fixture_ready_count}/{len(fixture_rows)} adapted INSERT fixtures"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
