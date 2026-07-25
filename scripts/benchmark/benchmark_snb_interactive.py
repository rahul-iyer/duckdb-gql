#!/usr/bin/env python3
"""Load and benchmark the LDBC SNB Interactive v1 DuckDB SQL queries.

The input is the official CsvBasic/LongDateFormatter dataset.  The official
DuckDB reference implementation expects CsvMergeForeign, so this harness first
materializes its relational schema from the separate CsvBasic relationship
files.  It then renders the reference SQL templates with SF10 substitution
parameters and records per-query warm latency.

This is a single-process engineering run, not an official LDBC driver score.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import platform
import re
import statistics
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


TIMER_RE = re.compile(r"Run Time \(s\): real ([0-9.]+)")


def sql_literal(value: Path | str) -> str:
    return "'" + str(value).replace("'", "''") + "'"


def csv_scan(path: Path) -> str:
    return (
        f"read_csv({sql_literal(path)}, delim='|', header=true, "
        "auto_detect=true, sample_size=20480)"
    )


def summarize(values: list[float]) -> dict[str, float]:
    return {
        "median": statistics.median(values),
        "min": min(values),
        "max": max(values),
        "mean": statistics.mean(values),
        "stdev": statistics.stdev(values) if len(values) > 1 else 0.0,
    }


def run_cli(
    cli: Path, database: Path, sql: str
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            str(cli),
            "-unsigned",
            "-no-init",
            "-storage-version",
            "v1.5.0",
            str(database),
        ],
        input=sql,
        text=True,
        capture_output=True,
        check=False,
    )


def dataset_paths(dataset: Path) -> dict[str, Path]:
    paths: dict[str, Path] = {}
    for directory in (dataset / "dynamic", dataset / "static"):
        for path in directory.glob("*.csv"):
            paths[path.stem.removesuffix("_0_0")] = path
    return paths


def load_sql(dataset: Path, threads: int, memory_limit: str) -> str:
    p = dataset_paths(dataset)
    required = {
        "person",
        "person_email_emailaddress",
        "person_hasInterest_tag",
        "person_isLocatedIn_place",
        "person_knows_person",
        "person_likes_post",
        "person_likes_comment",
        "person_speaks_language",
        "person_studyAt_organisation",
        "person_workAt_organisation",
        "forum",
        "forum_containerOf_post",
        "forum_hasMember_person",
        "forum_hasModerator_person",
        "forum_hasTag_tag",
        "post",
        "post_hasCreator_person",
        "post_hasTag_tag",
        "post_isLocatedIn_place",
        "comment",
        "comment_hasCreator_person",
        "comment_hasTag_tag",
        "comment_isLocatedIn_place",
        "comment_replyOf_post",
        "comment_replyOf_comment",
        "organisation",
        "organisation_isLocatedIn_place",
        "place",
        "place_isPartOf_place",
        "tag",
        "tag_hasType_tagclass",
        "tagclass",
        "tagclass_isSubclassOf_tagclass",
    }
    missing = sorted(required.difference(p))
    if missing:
        raise RuntimeError(f"missing CsvBasic inputs: {missing}")

    scan = {name: csv_scan(path) for name, path in p.items()}
    return f"""
PRAGMA threads={threads};
SET memory_limit={sql_literal(memory_limit)};
SET preserve_insertion_order=false;
.timer on

CREATE TABLE person AS
SELECT v.id::BIGINT AS p_personid,
       v.firstName::VARCHAR AS p_firstname,
       v.lastName::VARCHAR AS p_lastname,
       v.gender::VARCHAR AS p_gender,
       epoch_ms(v.birthday)::DATE AS p_birthday,
       epoch_ms(v.creationDate) AS p_creationdate,
       v.locationIP::VARCHAR AS p_locationip,
       v.browserUsed::VARCHAR AS p_browserused,
       location."Place.id"::BIGINT AS p_placeid
FROM {scan["person"]} v
JOIN {scan["person_isLocatedIn_place"]} location
  ON location."Person.id" = v.id;

CREATE TABLE person_email AS
SELECT "Person.id"::BIGINT AS pe_personid, email::VARCHAR AS pe_email
FROM {scan["person_email_emailaddress"]};

CREATE TABLE person_language AS
SELECT "Person.id"::BIGINT AS plang_personid, language::VARCHAR AS plang_language
FROM {scan["person_speaks_language"]};

CREATE TABLE person_tag AS
SELECT "Person.id"::BIGINT AS pt_personid, "Tag.id"::BIGINT AS pt_tagid
FROM {scan["person_hasInterest_tag"]};

CREATE TABLE person_university AS
SELECT "Person.id"::BIGINT AS pu_personid,
       "Organisation.id"::BIGINT AS pu_organisationid,
       classYear::INTEGER AS pu_classyear
FROM {scan["person_studyAt_organisation"]};

CREATE TABLE person_company AS
SELECT "Person.id"::BIGINT AS pc_personid,
       "Organisation.id"::BIGINT AS pc_organisationid,
       workFrom::INTEGER AS pc_workfrom
FROM {scan["person_workAt_organisation"]};

CREATE TABLE knows AS
SELECT "Person.id"::BIGINT AS k_person1id,
       "Person.id_1"::BIGINT AS k_person2id,
       epoch_ms(creationDate) AS k_creationdate
FROM {scan["person_knows_person"]}
UNION ALL
SELECT "Person.id_1"::BIGINT AS k_person1id,
       "Person.id"::BIGINT AS k_person2id,
       epoch_ms(creationDate) AS k_creationdate
FROM {scan["person_knows_person"]};

CREATE TABLE place AS
SELECT v.id::BIGINT AS pl_placeid,
       v.name::VARCHAR AS pl_name,
       v.url::VARCHAR AS pl_url,
       v.type::VARCHAR AS pl_type,
       parent."Place.id_1"::BIGINT AS pl_containerplaceid
FROM {scan["place"]} v
LEFT JOIN {scan["place_isPartOf_place"]} parent
  ON parent."Place.id" = v.id;

CREATE TABLE organisation AS
SELECT v.id::BIGINT AS o_organisationid,
       v.type::VARCHAR AS o_type,
       v.name::VARCHAR AS o_name,
       v.url::VARCHAR AS o_url,
       location."Place.id"::BIGINT AS o_placeid
FROM {scan["organisation"]} v
JOIN {scan["organisation_isLocatedIn_place"]} location
  ON location."Organisation.id" = v.id;

CREATE TABLE tagclass AS
SELECT v.id::BIGINT AS tc_tagclassid,
       v.name::VARCHAR AS tc_name,
       v.url::VARCHAR AS tc_url,
       parent."TagClass.id_1"::BIGINT AS tc_subclassoftagclassid
FROM {scan["tagclass"]} v
LEFT JOIN {scan["tagclass_isSubclassOf_tagclass"]} parent
  ON parent."TagClass.id" = v.id;

CREATE TABLE tag AS
SELECT v.id::BIGINT AS t_tagid,
       v.name::VARCHAR AS t_name,
       v.url::VARCHAR AS t_url,
       type."TagClass.id"::BIGINT AS t_tagclassid
FROM {scan["tag"]} v
JOIN {scan["tag_hasType_tagclass"]} type
  ON type."Tag.id" = v.id;

CREATE TABLE forum AS
SELECT v.id::BIGINT AS f_forumid,
       v.title::VARCHAR AS f_title,
       epoch_ms(v.creationDate) AS f_creationdate,
       moderator."Person.id"::BIGINT AS f_moderatorid
FROM {scan["forum"]} v
JOIN {scan["forum_hasModerator_person"]} moderator
  ON moderator."Forum.id" = v.id;

CREATE TABLE forum_person AS
SELECT "Forum.id"::BIGINT AS fp_forumid,
       "Person.id"::BIGINT AS fp_personid,
       epoch_ms(joinDate) AS fp_joindate
FROM {scan["forum_hasMember_person"]};

CREATE TABLE forum_tag AS
SELECT "Forum.id"::BIGINT AS ft_forumid, "Tag.id"::BIGINT AS ft_tagid
FROM {scan["forum_hasTag_tag"]};

CREATE TABLE likes AS
SELECT "Person.id"::BIGINT AS l_personid,
       "Post.id"::BIGINT AS l_messageid,
       epoch_ms(creationDate) AS l_creationdate
FROM {scan["person_likes_post"]}
UNION ALL
SELECT "Person.id"::BIGINT AS l_personid,
       "Comment.id"::BIGINT AS l_messageid,
       epoch_ms(creationDate) AS l_creationdate
FROM {scan["person_likes_comment"]};

CREATE TABLE message_tag AS
SELECT "Post.id"::BIGINT AS mt_messageid, "Tag.id"::BIGINT AS mt_tagid
FROM {scan["post_hasTag_tag"]}
UNION ALL
SELECT "Comment.id"::BIGINT AS mt_messageid, "Tag.id"::BIGINT AS mt_tagid
FROM {scan["comment_hasTag_tag"]};

CREATE TABLE message AS
SELECT v.id::BIGINT AS m_messageid,
       v.imageFile::VARCHAR AS m_ps_imagefile,
       epoch_ms(v.creationDate) AS m_creationdate,
       v.locationIP::VARCHAR AS m_locationip,
       v.browserUsed::VARCHAR AS m_browserused,
       v.language::VARCHAR AS m_ps_language,
       v.content::VARCHAR AS m_content,
       v.length::INTEGER AS m_length,
       creator."Person.id"::BIGINT AS m_creatorid,
       location."Place.id"::BIGINT AS m_locationid,
       container."Forum.id"::BIGINT AS m_ps_forumid,
       NULL::BIGINT AS m_c_replyof
FROM {scan["post"]} v
JOIN {scan["post_hasCreator_person"]} creator
  ON creator."Post.id" = v.id
JOIN {scan["post_isLocatedIn_place"]} location
  ON location."Post.id" = v.id
JOIN {scan["forum_containerOf_post"]} container
  ON container."Post.id" = v.id
UNION ALL
SELECT v.id::BIGINT AS m_messageid,
       NULL::VARCHAR AS m_ps_imagefile,
       epoch_ms(v.creationDate) AS m_creationdate,
       v.locationIP::VARCHAR AS m_locationip,
       v.browserUsed::VARCHAR AS m_browserused,
       NULL::VARCHAR AS m_ps_language,
       v.content::VARCHAR AS m_content,
       v.length::INTEGER AS m_length,
       creator."Person.id"::BIGINT AS m_creatorid,
       location."Place.id"::BIGINT AS m_locationid,
       NULL::BIGINT AS m_ps_forumid,
       coalesce(reply_post."Post.id", reply_comment."Comment.id_1")::BIGINT
           AS m_c_replyof
FROM {scan["comment"]} v
JOIN {scan["comment_hasCreator_person"]} creator
  ON creator."Comment.id" = v.id
JOIN {scan["comment_isLocatedIn_place"]} location
  ON location."Comment.id" = v.id
LEFT JOIN {scan["comment_replyOf_post"]} reply_post
  ON reply_post."Comment.id" = v.id
LEFT JOIN {scan["comment_replyOf_comment"]} reply_comment
  ON reply_comment."Comment.id" = v.id;

CREATE TABLE country AS
SELECT city.pl_placeid AS ctry_city, country.pl_name AS ctry_name
FROM place city
JOIN place country ON city.pl_containerplaceid = country.pl_placeid
WHERE country.pl_type = 'country';

CHECKPOINT;
"""


INDEX_SQL = """
CREATE UNIQUE INDEX forum_id_idx        ON forum       (f_forumid);
CREATE UNIQUE INDEX organisation_id_idx ON organisation(o_organisationid);
CREATE UNIQUE INDEX person_id_idx       ON person      (p_personid);
CREATE UNIQUE INDEX place_id_idx        ON place       (pl_placeid);
CREATE UNIQUE INDEX tagclass_id_idx     ON tagclass    (tc_tagclassid);
CREATE UNIQUE INDEX tag_id_idx          ON tag         (t_tagid);
CREATE UNIQUE INDEX message_id_idx      ON message     (m_messageid);
CREATE INDEX forum_moderatorid ON forum (f_moderatorid);
CREATE INDEX forum_person_forumid ON forum_person (fp_forumid);
CREATE INDEX forum_person_personid ON forum_person (fp_personid);
CREATE INDEX forum_tag_forumid ON forum_tag (ft_forumid);
CREATE INDEX forum_tag_tagid ON forum_tag (ft_tagid);
CREATE INDEX knows_person1id ON knows (k_person1id);
CREATE INDEX knows_person2id ON knows (k_person2id);
CREATE INDEX likes_personid ON likes (l_personid);
CREATE INDEX likes_messageid ON likes (l_messageid);
CREATE INDEX organisation_placeid ON organisation (o_placeid);
CREATE INDEX person_placeid ON person (p_placeid);
CREATE INDEX person_company_personid ON person_company (pc_personid);
CREATE INDEX person_company_organisationid ON person_company (pc_organisationid);
CREATE INDEX person_email_personid ON person_email (pe_personid);
CREATE INDEX person_language_personid ON person_language (plang_personid);
CREATE INDEX person_tag_personid ON person_tag (pt_personid);
CREATE INDEX person_tag_tagid ON person_tag (pt_tagid);
CREATE INDEX person_university_personid ON person_university (pu_personid);
CREATE INDEX person_university_organisationid ON person_university (pu_organisationid);
CREATE INDEX place_containerplaceid ON place (pl_containerplaceid);
CREATE INDEX message_creatorid ON message (m_creatorid);
CREATE INDEX message_locationid ON message (m_locationid);
CREATE INDEX message_forumid ON message (m_ps_forumid);
CREATE INDEX message_replyof ON message (m_c_replyof);
CREATE INDEX message_tag_messageid ON message_tag (mt_messageid);
CREATE INDEX message_tag_tagid ON message_tag (mt_tagid);
CREATE INDEX tag_tagclassid ON tag (t_tagclassid);
CREATE INDEX tagclass_subclassoftagclassid ON tagclass (tc_subclassoftagclassid);
CHECKPOINT;
"""


EXPECTED_COUNTS = {
    "person": 65_645,
    "place": 1_460,
    "organisation": 7_955,
    "tagclass": 71,
    "tag": 16_080,
    "forum": 595_453,
    "knows": 3_877_032,
    "message": 29_301_171,
    "likes": 28_789_223,
    "message_tag": 36_339_895,
}


def inspect_counts(cli: Path, database: Path) -> dict[str, int]:
    if not database.exists():
        return {}
    union = " UNION ALL ".join(
        f"SELECT {sql_literal(table)}, count(*) FROM {table}"
        for table in EXPECTED_COUNTS
    )
    completed = run_cli(cli, database, f".mode csv\n.headers off\n{union};\n")
    if completed.returncode:
        return {}
    return {
        row[0]: int(row[1])
        for row in csv.reader(completed.stdout.splitlines())
        if len(row) == 2
    }


def ensure_database(
    cli: Path,
    database: Path,
    dataset: Path,
    threads: int,
    memory_limit: str,
    create_indexes: bool,
) -> dict[str, Any]:
    counts = inspect_counts(cli, database)
    if counts == EXPECTED_COUNTS:
        return {"reused": True, "counts": counts}
    if database.exists():
        raise RuntimeError(
            f"{database} exists but has unexpected counts: {counts}; "
            "choose a new --database path"
        )
    database.parent.mkdir(parents=True, exist_ok=True)
    started = time.perf_counter()
    print("[load] materializing CsvBasic relational schema", flush=True)
    completed = run_cli(
        cli, database, load_sql(dataset, threads, memory_limit)
    )
    if completed.returncode:
        raise RuntimeError(
            f"relational load failed\nstdout:\n{completed.stdout}"
            f"\nstderr:\n{completed.stderr}"
        )
    load_seconds = time.perf_counter() - started
    index_seconds = 0.0
    if create_indexes:
        print("[load] creating reference indexes", flush=True)
        started = time.perf_counter()
        completed = run_cli(
            cli,
            database,
            f"PRAGMA threads={threads};\n"
            f"SET memory_limit={sql_literal(memory_limit)};\n{INDEX_SQL}",
        )
        if completed.returncode:
            raise RuntimeError(
                f"index creation failed\nstdout:\n{completed.stdout}"
                f"\nstderr:\n{completed.stderr}"
            )
        index_seconds = time.perf_counter() - started
    counts = inspect_counts(cli, database)
    if counts != EXPECTED_COUNTS:
        raise RuntimeError(
            f"loaded row counts do not match SF10: expected {EXPECTED_COUNTS}, "
            f"found {counts}"
        )
    return {
        "reused": False,
        "counts": counts,
        "load_seconds": load_seconds,
        "index_seconds": index_seconds,
    }


def read_parameter_row(parameters: Path, query_number: int) -> dict[str, str]:
    path = parameters / f"interactive_{query_number}_param.txt"
    with path.open(newline="") as source:
        row = next(csv.DictReader(source, delimiter="|"), None)
    if row is None:
        raise RuntimeError(f"no parameters in {path}")
    return dict(row)


STRING_PARAMETERS = {
    "firstName",
    "countryXName",
    "countryYName",
    "countryName",
    "tagName",
    "tagClassName",
}
DATE_PARAMETERS = {"maxDate", "startDate", "minDate"}


def render_reference_query(template: str, parameters: dict[str, str]) -> str:
    query = template.strip()
    query = query.replace("':durationDays days'", f"'{parameters.get('durationDays', '0')} days'")
    for name, raw in sorted(parameters.items(), key=lambda item: -len(item[0])):
        if name == "durationDays":
            replacement = raw
        elif name in STRING_PARAMETERS:
            replacement = sql_literal(raw)
        elif name in DATE_PARAMETERS:
            replacement = f"epoch_ms({int(raw)})"
        else:
            replacement = str(int(raw))
        query = re.sub(rf"(?<!:):{re.escape(name)}\b", replacement, query)
    unresolved = sorted(set(re.findall(r"(?<!:):([A-Za-z][A-Za-z0-9_]*)", query)))
    if unresolved:
        raise RuntimeError(f"unresolved parameters: {unresolved}")
    return query.rstrip().rstrip(";") + ";"


def profile_query(
    cli: Path,
    database: Path,
    query: str,
    threads: int,
    memory_limit: str,
    warmups: int,
    runs: int,
) -> dict[str, Any]:
    validation = run_cli(
        cli, database, ".mode csv\n.headers on\n" + query + "\n"
    )
    if validation.returncode:
        raise RuntimeError(validation.stderr.strip() or validation.stdout.strip())
    result_rows = max(0, len(validation.stdout.splitlines()) - 1)
    result_sha256 = hashlib.sha256(validation.stdout.encode()).hexdigest()

    payload = (
        ".output /dev/null\n"
        "PRAGMA enable_profiling='json';\n"
        + (query + "\n") * (warmups + runs)
    )
    completed = run_cli(
        cli,
        database,
        f"PRAGMA threads={threads};\n"
        f"SET memory_limit={sql_literal(memory_limit)};\n{payload}",
    )
    if completed.returncode:
        raise RuntimeError(completed.stderr.strip() or completed.stdout.strip())
    decoder = json.JSONDecoder()
    profiles: list[dict[str, Any]] = []
    offset = 0
    while True:
        start = completed.stderr.find("{", offset)
        if start < 0:
            break
        try:
            value, end = decoder.raw_decode(completed.stderr, start)
        except json.JSONDecodeError:
            offset = start + 1
            continue
        if "latency" in value and "query_name" in value:
            profiles.append(value)
        offset = end
    if len(profiles) != warmups + runs:
        raise RuntimeError(
            f"expected {warmups + runs} profiles, found {len(profiles)}"
        )
    measured = profiles[warmups:]
    return {
        "status": "passed",
        "result_rows": result_rows,
        "result_sha256": result_sha256,
        "latency_seconds": summarize([entry["latency"] for entry in measured]),
        "cpu_seconds": summarize([entry["cpu_time"] for entry in measured]),
        "cumulative_rows_scanned": summarize(
            [float(entry["cumulative_rows_scanned"]) for entry in measured]
        ),
    }


def choose_short_parameters(cli: Path, database: Path, person_id: int) -> dict[int, dict[str, str]]:
    sql = """
.mode csv
.headers off
SELECT m_messageid
FROM message
WHERE EXISTS (
    SELECT 1 FROM message reply WHERE reply.m_c_replyof = message.m_messageid
)
ORDER BY m_messageid
LIMIT 1;
"""
    completed = run_cli(cli, database, sql)
    if completed.returncode or not completed.stdout.strip():
        raise RuntimeError("could not choose a short-read message seed")
    message_id = int(next(csv.reader(completed.stdout.splitlines()))[0])
    return {
        1: {"personId": str(person_id)},
        2: {"personId": str(person_id)},
        3: {"personId": str(person_id)},
        4: {"messageId": str(message_id)},
        5: {"messageId": str(message_id)},
        6: {"messageId": str(message_id)},
        7: {"messageId": str(message_id)},
    }


def benchmark_queries(
    cli: Path,
    database: Path,
    query_root: Path,
    parameters: Path,
    threads: int,
    memory_limit: str,
    warmups: int,
    runs: int,
) -> tuple[dict[str, Any], dict[str, Any]]:
    complex_parameters = {
        number: read_parameter_row(parameters, number)
        for number in range(1, 15)
    }
    seed_person = int(complex_parameters[1]["personId"])
    short_parameters = choose_short_parameters(cli, database, seed_person)
    results: dict[str, Any] = {}
    failures: dict[str, Any] = {}
    cases = [
        ("short", number, short_parameters[number]) for number in range(1, 8)
    ] + [
        ("complex", number, complex_parameters[number])
        for number in range(1, 15)
    ]
    for family, number, values in cases:
        name = f"interactive-{family}-{number}"
        template_path = query_root / f"{name}.sql"
        template = template_path.read_text()
        query = render_reference_query(template, values)
        print(f"[{name}] running", flush=True)
        try:
            result = profile_query(
                cli,
                database,
                query,
                threads,
                memory_limit,
                warmups,
                runs,
            )
            result["parameters"] = values
            result["template"] = str(template_path)
            results[name] = result
            print(
                f"[{name}] {result['latency_seconds']['median']:.6f}s, "
                f"{result['result_rows']} rows",
                flush=True,
            )
        except RuntimeError as error:
            failures[name] = {
                "status": "failed",
                "parameters": values,
                "template": str(template_path),
                "error": str(error),
            }
            print(f"[{name}] FAILED: {error}", flush=True)
    return results, failures


def source_metadata() -> dict[str, Any]:
    root = Path(__file__).resolve().parents[2]
    commit = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=root,
        text=True,
        capture_output=True,
        check=True,
    ).stdout.strip()
    dirty = bool(
        subprocess.run(
            ["git", "status", "--porcelain", "--untracked-files=no"],
            cwd=root,
            text=True,
            capture_output=True,
            check=True,
        ).stdout.strip()
    )
    return {"commit": commit, "dirty": dirty}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", type=Path, default=Path("build/release/duckdb"))
    parser.add_argument(
        "--dataset",
        type=Path,
        default=Path(
            "build/benchmarks/snb10/extracted/"
            "social_network-sf10-CsvBasic-LongDateFormatter"
        ),
    )
    parser.add_argument(
        "--parameters",
        type=Path,
        default=Path(
            "build/benchmarks/snb10/extracted/substitution_parameters-sf10"
        ),
    )
    parser.add_argument(
        "--reference",
        type=Path,
        default=Path(
            "build/benchmarks/snb10/ldbc_snb_interactive_v1_impls"
        ),
    )
    parser.add_argument(
        "--database",
        type=Path,
        default=Path("build/benchmarks/snb10/snb10-relational.duckdb"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("build/benchmarks/snb10/interactive-results.json"),
    )
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--memory-limit", default="20GB")
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--no-indexes", action="store_true")
    parser.add_argument("--load-only", action="store_true")
    args = parser.parse_args()

    if args.threads < 1 or args.warmups < 0 or args.runs < 1:
        raise SystemExit("threads/runs must be positive and warmups non-negative")
    cli = args.duckdb.resolve()
    dataset = args.dataset.resolve()
    parameters = args.parameters.resolve()
    reference = args.reference.resolve()
    database = args.database.resolve()
    output = args.output.resolve()
    query_root = reference / "duckdb" / "queries"
    for path in (cli, dataset, parameters, query_root):
        if not path.exists():
            raise SystemExit(f"missing required path: {path}")

    started = time.perf_counter()
    load = ensure_database(
        cli,
        database,
        dataset,
        args.threads,
        args.memory_limit,
        not args.no_indexes,
    )
    results: dict[str, Any] = {}
    failures: dict[str, Any] = {}
    if not args.load_only:
        results, failures = benchmark_queries(
            cli,
            database,
            query_root,
            parameters,
            args.threads,
            args.memory_limit,
            args.warmups,
            args.runs,
        )
    payload = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "benchmark_kind": (
            "LDBC SNB Interactive v1 reference SQL engineering run; "
            "not an official driver score"
        ),
        "source": source_metadata(),
        "reference_commit": subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=reference,
            text=True,
            capture_output=True,
            check=True,
        ).stdout.strip(),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "configuration": {
            "threads": args.threads,
            "memory_limit": args.memory_limit,
            "warmups": args.warmups,
            "runs": args.runs,
            "indexes": not args.no_indexes,
        },
        "dataset": dataset.name,
        "database": str(database),
        "database_bytes": database.stat().st_size,
        "load": load,
        "queries": results,
        "failures": failures,
        "total_wall_seconds": time.perf_counter() - started,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, indent=2) + "\n")
    print(
        json.dumps(
            {
                "output": str(output),
                "passed": len(results),
                "failed": len(failures),
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
