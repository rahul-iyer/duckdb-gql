#!/usr/bin/env python3
"""Build a complete SF10 DuckGQL graph and benchmark supported SNB reads.

The source database is produced by benchmark_snb_interactive.py.  This harness
exports one graph-header Parquet vertex file and one edge file, imports them
with COPY GRAPH, builds the CSR, and records reproducible engineering timings.
It is not an official LDBC driver score.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import platform
import statistics
import subprocess
import time
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any


EXPECTED_VERTICES = 29_987_835
EXPECTED_EDGES = 178_561_949

PERSON = 1_000_000_000_000_000
MESSAGE = 2_000_000_000_000_000
FORUM = 3_000_000_000_000_000
PLACE = 4_000_000_000_000_000
ORGANISATION = 5_000_000_000_000_000
TAG = 6_000_000_000_000_000
TAGCLASS = 7_000_000_000_000_000


def sql_literal(value: Path | str) -> str:
    return "'" + str(value).replace("'", "''") + "'"


def run_cli(
    cli: Path, database: Path, sql: str, timeout: float | None = None
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
        timeout=timeout,
    )


def scalar(cli: Path, database: Path, sql: str) -> str:
    completed = run_cli(cli, database, f".mode csv\n.headers off\n{sql}\n")
    if completed.returncode:
        raise RuntimeError(completed.stderr.strip() or completed.stdout.strip())
    rows = list(csv.reader(completed.stdout.splitlines()))
    if len(rows) != 1 or len(rows[0]) != 1:
        raise RuntimeError(f"expected one scalar result, found: {completed.stdout}")
    return rows[0][0]


def parquet_count(cli: Path, database: Path, path: Path) -> int | None:
    if not path.exists():
        return None
    try:
        return int(
            scalar(
                cli,
                database,
                f"SELECT count(*) FROM read_parquet({sql_literal(path)});",
            )
        )
    except RuntimeError:
        return None


def vertex_export_sql(path: Path, threads: int, memory_limit: str) -> str:
    nulls = {
        "text": "NULL::VARCHAR",
        "integer": "NULL::INTEGER",
        "date": "NULL::DATE",
        "timestamp": "NULL::TIMESTAMP",
        "list": "NULL::VARCHAR[]",
    }
    columns = [
        "firstName",
        "lastName",
        "gender",
        "birthday",
        "creationDate",
        "locationIP",
        "browserUsed",
        "email",
        "speaks",
        "imageFile",
        "language",
        "content",
        "length",
        "title",
        "name",
        "url",
        "type",
    ]
    kinds = {
        "firstName": "text",
        "lastName": "text",
        "gender": "text",
        "birthday": "date",
        "creationDate": "timestamp",
        "locationIP": "text",
        "browserUsed": "text",
        "email": "list",
        "speaks": "list",
        "imageFile": "text",
        "language": "text",
        "content": "text",
        "length": "integer",
        "title": "text",
        "name": "text",
        "url": "text",
        "type": "text",
    }

    def projection(
        external_id: str,
        label: str,
        identifier: str,
        values: dict[str, str],
        source: str,
    ) -> str:
        properties = ",\n       ".join(
            f"{values.get(name, nulls[kinds[name]])} AS \"{name}\""
            for name in columns
        )
        return (
            f"SELECT {external_id}::BIGINT AS \":ID\",\n"
            f"       {label}::VARCHAR AS \":LABEL\",\n"
            f"       {identifier}::BIGINT AS id,\n"
            f"       {properties}\n"
            f"FROM {source}"
        )

    person = projection(
        f"{PERSON} + p.p_personid",
        "'Person'",
        "p.p_personid",
        {
            "firstName": "p.p_firstname",
            "lastName": "p.p_lastname",
            "gender": "p.p_gender",
            "birthday": "p.p_birthday",
            "creationDate": "p.p_creationdate",
            "locationIP": "p.p_locationip",
            "browserUsed": "p.p_browserused",
            "email": "e.email",
            "speaks": "l.speaks",
        },
        """person p
LEFT JOIN (
    SELECT pe_personid, list(pe_email ORDER BY pe_email) AS email
    FROM person_email GROUP BY pe_personid
) e ON e.pe_personid = p.p_personid
LEFT JOIN (
    SELECT plang_personid, list(plang_language ORDER BY plang_language) AS speaks
    FROM person_language GROUP BY plang_personid
) l ON l.plang_personid = p.p_personid""",
    )
    message = projection(
        f"{MESSAGE} + m_messageid",
        "CASE WHEN m_ps_forumid IS NULL THEN 'Comment;Message' ELSE 'Post;Message' END",
        "m_messageid",
        {
            "creationDate": "m_creationdate",
            "locationIP": "m_locationip",
            "browserUsed": "m_browserused",
            "imageFile": "m_ps_imagefile",
            "language": "m_ps_language",
            "content": "m_content",
            "length": "m_length",
        },
        "message",
    )
    forum = projection(
        f"{FORUM} + f_forumid",
        "'Forum'",
        "f_forumid",
        {"creationDate": "f_creationdate", "title": "f_title"},
        "forum",
    )
    place = projection(
        f"{PLACE} + pl_placeid",
        "'Place;' || upper(pl_type[1]) || pl_type[2:]",
        "pl_placeid",
        {"name": "pl_name", "url": "pl_url", "type": "pl_type"},
        "place",
    )
    organisation = projection(
        f"{ORGANISATION} + o_organisationid",
        "'Organisation;' || upper(o_type[1]) || o_type[2:]",
        "o_organisationid",
        {"name": "o_name", "url": "o_url", "type": "o_type"},
        "organisation",
    )
    tag = projection(
        f"{TAG} + t_tagid",
        "'Tag'",
        "t_tagid",
        {"name": "t_name", "url": "t_url"},
        "tag",
    )
    tagclass = projection(
        f"{TAGCLASS} + tc_tagclassid",
        "'TagClass'",
        "tc_tagclassid",
        {"name": "tc_name", "url": "tc_url"},
        "tagclass",
    )
    union = "\nUNION ALL BY NAME\n".join(
        [person, message, forum, place, organisation, tag, tagclass]
    )
    return f"""
PRAGMA threads={threads};
SET memory_limit={sql_literal(memory_limit)};
SET preserve_insertion_order=false;
COPY (
{union}
) TO {sql_literal(path)}
  (FORMAT PARQUET, COMPRESSION ZSTD, ROW_GROUP_SIZE 122880);
"""


def edge_export_sql(path: Path, threads: int, memory_limit: str) -> str:
    def edge(
        source: str,
        target: str,
        label: str,
        relation: str,
        creation: str = "NULL::TIMESTAMP",
        join_date: str = "NULL::TIMESTAMP",
        class_year: str = "NULL::INTEGER",
        work_from: str = "NULL::INTEGER",
        where: str = "",
    ) -> str:
        suffix = f"\nWHERE {where}" if where else ""
        return f"""SELECT ({source})::BIGINT AS ":START_ID",
       ({target})::BIGINT AS ":END_ID",
       '{label}'::VARCHAR AS ":TYPE",
       {creation} AS "creationDate",
       {join_date} AS "joinDate",
       {class_year} AS "classYear",
       {work_from} AS "workFrom"
FROM {relation}{suffix}"""

    relations = [
        edge(
            f"{PERSON} + p_personid",
            f"{PLACE} + p_placeid",
            "IS_LOCATED_IN",
            "person",
        ),
        edge(
            f"{MESSAGE} + m_messageid",
            f"{PLACE} + m_locationid",
            "IS_LOCATED_IN",
            "message",
        ),
        edge(
            f"{ORGANISATION} + o_organisationid",
            f"{PLACE} + o_placeid",
            "IS_LOCATED_IN",
            "organisation",
        ),
        edge(
            f"{PERSON} + pt_personid",
            f"{TAG} + pt_tagid",
            "HAS_INTEREST",
            "person_tag",
        ),
        edge(
            f"{PERSON} + pu_personid",
            f"{ORGANISATION} + pu_organisationid",
            "STUDY_AT",
            "person_university",
            class_year="pu_classyear",
        ),
        edge(
            f"{PERSON} + pc_personid",
            f"{ORGANISATION} + pc_organisationid",
            "WORK_AT",
            "person_company",
            work_from="pc_workfrom",
        ),
        edge(
            f"{PERSON} + k_person1id",
            f"{PERSON} + k_person2id",
            "KNOWS",
            "knows",
            creation="k_creationdate",
        ),
        edge(
            f"{FORUM} + m_ps_forumid",
            f"{MESSAGE} + m_messageid",
            "CONTAINER_OF",
            "message",
            where="m_ps_forumid IS NOT NULL",
        ),
        edge(
            f"{FORUM} + fp_forumid",
            f"{PERSON} + fp_personid",
            "HAS_MEMBER",
            "forum_person",
            join_date="fp_joindate",
        ),
        edge(
            f"{FORUM} + f_forumid",
            f"{PERSON} + f_moderatorid",
            "HAS_MODERATOR",
            "forum",
        ),
        edge(
            f"{FORUM} + ft_forumid",
            f"{TAG} + ft_tagid",
            "HAS_TAG",
            "forum_tag",
        ),
        edge(
            f"{PERSON} + l_personid",
            f"{MESSAGE} + l_messageid",
            "LIKES",
            "likes",
            creation="l_creationdate",
        ),
        edge(
            f"{MESSAGE} + m_messageid",
            f"{PERSON} + m_creatorid",
            "HAS_CREATOR",
            "message",
        ),
        edge(
            f"{MESSAGE} + mt_messageid",
            f"{TAG} + mt_tagid",
            "HAS_TAG",
            "message_tag",
        ),
        edge(
            f"{MESSAGE} + m_messageid",
            f"{MESSAGE} + m_c_replyof",
            "REPLY_OF",
            "message",
            where="m_c_replyof IS NOT NULL",
        ),
        edge(
            f"{TAG} + t_tagid",
            f"{TAGCLASS} + t_tagclassid",
            "HAS_TYPE",
            "tag",
        ),
        edge(
            f"{TAGCLASS} + tc_tagclassid",
            f"{TAGCLASS} + tc_subclassoftagclassid",
            "IS_SUBCLASS_OF",
            "tagclass",
            where="tc_subclassoftagclassid IS NOT NULL",
        ),
        edge(
            f"{PLACE} + pl_placeid",
            f"{PLACE} + pl_containerplaceid",
            "IS_PART_OF",
            "place",
            where="pl_containerplaceid IS NOT NULL",
        ),
    ]
    union = "\nUNION ALL\n".join(relations)
    return f"""
PRAGMA threads={threads};
SET memory_limit={sql_literal(memory_limit)};
SET preserve_insertion_order=false;
COPY (
{union}
) TO {sql_literal(path)}
  (FORMAT PARQUET, COMPRESSION ZSTD, ROW_GROUP_SIZE 122880);
"""


def ensure_parquet(
    cli: Path,
    source_database: Path,
    vertices: Path,
    edges: Path,
    threads: int,
    memory_limit: str,
) -> dict[str, Any]:
    vertices.parent.mkdir(parents=True, exist_ok=True)
    timings: dict[str, float] = {}
    counts = {
        "vertices": parquet_count(cli, source_database, vertices),
        "edges": parquet_count(cli, source_database, edges),
    }
    for name, path, expected, statement in (
        (
            "vertices",
            vertices,
            EXPECTED_VERTICES,
            vertex_export_sql(vertices, threads, memory_limit),
        ),
        (
            "edges",
            edges,
            EXPECTED_EDGES,
            edge_export_sql(edges, threads, memory_limit),
        ),
    ):
        if counts[name] == expected:
            print(f"[export] reusing {name}: {path}", flush=True)
            continue
        if path.exists():
            raise RuntimeError(
                f"{path} has {counts[name]} rows, expected {expected}; "
                "move it aside before retrying"
            )
        print(f"[export] writing {expected:,} {name}", flush=True)
        started = time.perf_counter()
        completed = run_cli(cli, source_database, statement)
        if completed.returncode:
            raise RuntimeError(
                f"{name} export failed\nstdout:\n{completed.stdout}"
                f"\nstderr:\n{completed.stderr}"
            )
        timings[f"{name}_seconds"] = time.perf_counter() - started
        counts[name] = parquet_count(cli, source_database, path)
        if counts[name] != expected:
            raise RuntimeError(
                f"{name} export has {counts[name]} rows, expected {expected}"
            )
    return {
        "counts": counts,
        "timings": timings,
        "bytes": {
            "vertices": vertices.stat().st_size,
            "edges": edges.stat().st_size,
        },
    }


def inspect_graph(cli: Path, database: Path) -> tuple[int, int] | None:
    if not database.exists():
        return None
    try:
        completed = run_cli(
            cli,
            database,
            """.mode csv
.headers off
SELECT vertex_count, edge_count
FROM gql_graphs()
WHERE graph_name = 'snb_interactive';
""",
        )
        if completed.returncode:
            return None
        rows = list(csv.reader(completed.stdout.splitlines()))
        if len(rows) != 1:
            return None
        return int(rows[0][0]), int(rows[0][1])
    except (RuntimeError, ValueError):
        return None


def ensure_graph(
    cli: Path,
    database: Path,
    vertices: Path,
    edges: Path,
    threads: int,
    memory_limit: str,
) -> dict[str, Any]:
    existing = inspect_graph(cli, database)
    expected = (EXPECTED_VERTICES, EXPECTED_EDGES)
    if existing == expected:
        statement = f"""
PRAGMA threads={threads};
SET memory_limit={sql_literal(memory_limit)};
SET preserve_insertion_order=false;
.once /dev/null
CALL gql_create_property_index('snb_interactive', 'id');
.once /dev/null
CALL gql_create_property_index('snb_interactive', 'name');
CHECKPOINT;
"""
        started = time.perf_counter()
        completed = run_cli(cli, database, statement)
        if completed.returncode:
            raise RuntimeError(
                f"property index creation failed\nstdout:\n{completed.stdout}"
                f"\nstderr:\n{completed.stderr}"
            )
        return {
            "reused": True,
            "counts": existing,
            "property_index_seconds": time.perf_counter() - started,
        }
    if database.exists():
        raise RuntimeError(
            f"{database} contains graph counts {existing}, expected {expected}; "
            "choose a new --graph-database path"
        )
    database.parent.mkdir(parents=True, exist_ok=True)
    statement = f"""
PRAGMA threads={threads};
SET memory_limit={sql_literal(memory_limit)};
SET preserve_insertion_order=false;
CREATE GRAPH snb_interactive ANY;
COPY GRAPH snb_interactive FROM (
    VERTICES {sql_literal(vertices)},
    EDGES {sql_literal(edges)}
) FORMAT GRAPH OPTIONS (VALIDATE FALSE);
CALL gql_create_property_index('snb_interactive', 'id');
CALL gql_create_property_index('snb_interactive', 'name');
CHECKPOINT;
"""
    print("[graph] COPY GRAPH and property index build", flush=True)
    started = time.perf_counter()
    completed = run_cli(cli, database, statement)
    if completed.returncode:
        raise RuntimeError(
            f"graph load failed\nstdout:\n{completed.stdout}"
            f"\nstderr:\n{completed.stderr}"
        )
    elapsed = time.perf_counter() - started
    actual = inspect_graph(cli, database)
    if actual != expected:
        raise RuntimeError(f"graph has counts {actual}, expected {expected}")
    return {
        "reused": False,
        "counts": actual,
        "load_and_property_index_seconds": elapsed,
    }


def summarize(values: list[float]) -> dict[str, float]:
    return {
        "median": statistics.median(values),
        "min": min(values),
        "max": max(values),
        "mean": statistics.mean(values),
        "stdev": statistics.stdev(values) if len(values) > 1 else 0.0,
    }


def parse_profiles(stderr: str) -> list[dict[str, Any]]:
    decoder = json.JSONDecoder()
    profiles: list[dict[str, Any]] = []
    offset = 0
    while True:
        start = stderr.find("{", offset)
        if start < 0:
            break
        try:
            value, end = decoder.raw_decode(stderr, start)
        except json.JSONDecodeError:
            offset = start + 1
            continue
        if "latency" in value and "query_name" in value:
            profiles.append(value)
        offset = end
    return profiles


def query_rows(
    cli: Path,
    database: Path,
    query: str,
    graph: bool,
    timeout: float,
    threads: int,
    memory_limit: str,
) -> tuple[list[list[str]], dict[str, Any] | None]:
    prefix = (
        f"PRAGMA threads={threads};\n"
        f"SET memory_limit={sql_literal(memory_limit)};\n"
        "SET preserve_insertion_order=false;\n"
    )
    if graph:
        prefix += (
            ".once /dev/null\n"
            "CALL gql_build_csr('snb_interactive');\n"
            ".once /dev/null\n"
            "SESSION SET GRAPH snb_interactive;\n"
            "PRAGMA enable_profiling='json';\n"
        )
    completed = run_cli(
        cli,
        database,
        ".mode csv\n.headers off\n" + prefix + query.strip() + "\n",
        timeout=timeout,
    )
    if completed.returncode:
        raise RuntimeError(completed.stderr.strip() or completed.stdout.strip())
    profiles = parse_profiles(completed.stderr)
    profile = profiles[-1] if graph and profiles else None
    return list(csv.reader(completed.stdout.splitlines())), profile


def benchmark_repeated(
    cli: Path,
    database: Path,
    query: str,
    threads: int,
    memory_limit: str,
    warmups: int,
    runs: int,
    timeout: float,
) -> dict[str, Any]:
    statements = "\n".join(query.strip() for _ in range(warmups + runs))
    sql = f"""
PRAGMA threads={threads};
SET memory_limit={sql_literal(memory_limit)};
SET preserve_insertion_order=false;
.once /dev/null
CALL gql_build_csr('snb_interactive');
.once /dev/null
SESSION SET GRAPH snb_interactive;
.output /dev/null
PRAGMA enable_profiling='json';
{statements}
"""
    completed = run_cli(cli, database, sql, timeout=timeout)
    if completed.returncode:
        raise RuntimeError(completed.stderr.strip() or completed.stdout.strip())
    profiles = parse_profiles(completed.stderr)
    if len(profiles) != warmups + runs:
        raise RuntimeError(
            f"expected {warmups + runs} profiles, found {len(profiles)}"
        )
    measured = profiles[warmups:]
    return {
        "warmups": warmups,
        "runs": runs,
        "latency_seconds": summarize([p["latency"] for p in measured]),
        "cpu_seconds": summarize([p["cpu_time"] for p in measured]),
        "cumulative_rows_scanned": summarize(
            [float(p["cumulative_rows_scanned"]) for p in measured]
        ),
    }


def benchmark_cases(
    cli: Path,
    relational_database: Path,
    graph_database: Path,
    relational_results: Path,
    threads: int,
    memory_limit: str,
    warmups: int,
    runs: int,
    timeout: float,
    selected_queries: set[str] | None = None,
) -> tuple[dict[str, Any], dict[str, Any]]:
    reference = json.loads(relational_results.read_text())
    params = {
        name: entry["parameters"]
        for name, entry in reference["queries"].items()
    }
    person_id = int(params["interactive-short-1"]["personId"])
    message_id = int(params["interactive-short-4"]["messageId"])
    short2_person = int(params["interactive-short-2"]["personId"])
    short6_message = int(params["interactive-short-6"]["messageId"])
    complex2_person = int(params["interactive-complex-2"]["personId"])
    complex2_date = datetime.fromtimestamp(
        int(params["interactive-complex-2"]["maxDate"]) / 1000,
        tz=timezone.utc,
    ).replace(tzinfo=None)
    complex3_person = int(params["interactive-complex-3"]["personId"])
    complex3_start = datetime.fromtimestamp(
        int(params["interactive-complex-3"]["startDate"]) / 1000,
        tz=timezone.utc,
    ).replace(tzinfo=None)
    complex3_end = complex3_start + timedelta(
        days=int(params["interactive-complex-3"]["durationDays"])
    )
    complex3_country_x = str(params["interactive-complex-3"]["countryXName"])
    complex3_country_y = str(params["interactive-complex-3"]["countryYName"])
    complex4_person = int(params["interactive-complex-4"]["personId"])
    complex4_start = datetime.fromtimestamp(
        int(params["interactive-complex-4"]["startDate"]) / 1000,
        tz=timezone.utc,
    ).replace(tzinfo=None)
    complex4_end = complex4_start + timedelta(
        days=int(params["interactive-complex-4"]["durationDays"])
    )
    complex5_person = int(params["interactive-complex-5"]["personId"])
    complex5_date = datetime.fromtimestamp(
        int(params["interactive-complex-5"]["minDate"]) / 1000,
        tz=timezone.utc,
    ).replace(tzinfo=None)
    complex6_person = int(params["interactive-complex-6"]["personId"])
    complex6_tag = str(params["interactive-complex-6"]["tagName"])
    complex8_person = int(params["interactive-complex-8"]["personId"])
    complex9_person = int(params["interactive-complex-9"]["personId"])
    complex9_date = datetime.fromtimestamp(
        int(params["interactive-complex-9"]["maxDate"]) / 1000,
        tz=timezone.utc,
    ).replace(tzinfo=None)
    complex11_person = int(params["interactive-complex-11"]["personId"])
    complex11_country = str(params["interactive-complex-11"]["countryName"])
    complex11_year = int(params["interactive-complex-11"]["workFromYear"])
    complex13_person1 = int(params["interactive-complex-13"]["person1Id"])
    complex13_person2 = int(params["interactive-complex-13"]["person2Id"])

    cases = {
        "interactive-short-1": {
            "sql": f"""
SELECT p_firstname, p_lastname, p_birthday, p_locationip, p_browserused,
       p_placeid, p_gender, p_creationdate
FROM person WHERE p_personid = {person_id};
""",
            "gql": f"""
MATCH (p:Person)-[:IS_LOCATED_IN]->(place:Place)
WHERE p.id = {person_id}
RETURN p.firstName, p.lastName, p.birthday, p.locationIP, p.browserUsed,
       place.id, p.gender, p.creationDate;
""",
        },
        "interactive-short-2": {
            "sql": f"""
WITH RECURSIVE cposts AS (
    SELECT m_messageid, m_content, m_ps_imagefile, m_creationdate,
           m_c_replyof, m_creatorid
    FROM message
    WHERE m_creatorid = {short2_person}
    ORDER BY m_creationdate DESC
    LIMIT 10
), parent(postid, replyof, orig_postid, creator) AS (
    SELECT m_messageid, m_c_replyof, m_messageid, m_creatorid FROM cposts
    UNION ALL
    SELECT m_messageid, m_c_replyof, orig_postid, m_creatorid
    FROM message, parent
    WHERE m_messageid = replyof
)
SELECT p1.m_messageid, COALESCE(m_ps_imagefile, m_content, ''),
       p1.m_creationdate, p2.m_messageid, p2.p_personid,
       p2.p_firstname, p2.p_lastname
FROM (
    SELECT m_messageid, m_content, m_ps_imagefile, m_creationdate,
           m_c_replyof
    FROM cposts
) p1
LEFT JOIN (
    SELECT orig_postid, postid AS m_messageid, p_personid,
           p_firstname, p_lastname
    FROM parent, person
    WHERE replyof IS NULL AND creator = p_personid
) p2 ON p2.orig_postid = p1.m_messageid
ORDER BY m_creationdate DESC, p2.m_messageid DESC;
""",
            "gql": f"""
MATCH (person:Person)<-[:HAS_CREATOR]-(message:Message)
WHERE person.id = {short2_person}
MATCH (message)-[:REPLY_OF]->*(root:Post)
MATCH (root)-[:HAS_CREATOR]->(author:Person)
RETURN message.id, COALESCE(message.imageFile, message.content, ''),
       message.creationDate, root.id, author.id, author.firstName, author.lastName
ORDER BY message.creationDate DESC, root.id DESC
LIMIT 10;
""",
        },
        "interactive-short-3": {
            "sql": f"""
SELECT p_personid, p_firstname, p_lastname, k_creationdate
FROM knows, person
WHERE k_person1id = {person_id} AND k_person2id = p_personid
ORDER BY k_creationdate DESC, p_personid ASC;
""",
            "gql": f"""
MATCH (p:Person)-[k:KNOWS]->(friend:Person)
WHERE p.id = {person_id}
RETURN friend.id, friend.firstName, friend.lastName, k.creationDate
ORDER BY k.creationDate DESC, friend.id ASC;
""",
        },
        "interactive-short-4": {
            "sql": f"""
SELECT COALESCE(m_ps_imagefile, m_content, ''), m_creationdate
FROM message WHERE m_messageid = {message_id};
""",
            "gql": f"""
MATCH (m:Message) WHERE m.id = {message_id}
RETURN COALESCE(m.imageFile, m.content, ''), m.creationDate;
""",
        },
        "interactive-short-5": {
            "sql": f"""
SELECT p_personid, p_firstname, p_lastname
FROM message, person
WHERE m_messageid = {message_id} AND m_creatorid = p_personid;
""",
            "gql": f"""
MATCH (m:Message)-[:HAS_CREATOR]->(p:Person)
WHERE m.id = {message_id}
RETURN p.id, p.firstName, p.lastName;
""",
        },
        "interactive-short-6": {
            "sql": f"""
WITH RECURSIVE chain(parent, child) AS (
    SELECT m_c_replyof, m_messageid
    FROM message WHERE m_messageid = {short6_message}
    UNION ALL
    SELECT p.m_c_replyof, p.m_messageid
    FROM message p, chain c
    WHERE p.m_messageid = c.parent
)
SELECT f_forumid, f_title, p_personid, p_firstname, p_lastname
FROM person, forum, (
    SELECT m_messageid, m_ps_forumid
    FROM message
    WHERE m_messageid = (
        SELECT COALESCE(min(parent), {short6_message}) FROM chain
    )
) m
WHERE m_ps_forumid = f_forumid
  AND f_moderatorid = p_personid;
""",
            "gql": f"""
MATCH (message:Message) WHERE message.id = {short6_message}
MATCH (message)-[:REPLY_OF]->*(post:Post)
MATCH (forum:Forum)-[:CONTAINER_OF]->(post)
MATCH (forum)-[:HAS_MODERATOR]->(moderator:Person)
RETURN forum.id, forum.title, moderator.id,
       moderator.firstName, moderator.lastName;
""",
        },
        "interactive-short-7": {
            "sql": f"""
SELECT p2.m_messageid, p2.m_content, p2.m_creationdate,
       p_personid, p_firstname, p_lastname,
       CASE WHEN EXISTS (
           SELECT 1 FROM knows
           WHERE p1.m_creatorid = k_person1id
             AND p2.m_creatorid = k_person2id
       ) THEN TRUE ELSE FALSE END
FROM message p1, message p2, person
WHERE p1.m_messageid = {message_id}
  AND p2.m_c_replyof = p1.m_messageid
  AND p2.m_creatorid = p_personid
ORDER BY p2.m_creationdate DESC, p2.m_creatorid ASC;
""",
            "gql": f"""
MATCH (original:Message) WHERE original.id = {message_id}
MATCH (original)-[:HAS_CREATOR]->(original_creator:Person)
MATCH (reply:Comment)-[:REPLY_OF]->(original)
MATCH (reply)-[:HAS_CREATOR]->(reply_creator:Person)
OPTIONAL MATCH (original_creator)-[knows:KNOWS]->(reply_creator)
RETURN reply.id, reply.content, reply.creationDate,
       reply_creator.id, reply_creator.firstName, reply_creator.lastName,
       COUNT(knows) > 0
ORDER BY reply.creationDate DESC, reply_creator.id ASC;
""",
        },
        "interactive-complex-2": {
            "sql": f"""
SELECT p_personid, p_firstname, p_lastname, m_messageid,
       COALESCE(m_ps_imagefile, m_content), m_creationdate
FROM person, message, knows
WHERE p_personid = m_creatorid
  AND m_creationdate <= epoch_ms({int(params["interactive-complex-2"]["maxDate"])})
  AND k_person1id = {complex2_person}
  AND k_person2id = p_personid
ORDER BY m_creationdate DESC, m_messageid ASC
LIMIT 20;
""",
            "gql": f"""
MATCH (source:Person)-[:KNOWS]->(friend:Person),
      (message:Message)-[:HAS_CREATOR]->(friend)
WHERE source.id = {complex2_person}
  AND message.creationDate <= '{complex2_date:%Y-%m-%d %H:%M:%S}'
RETURN friend.id, friend.firstName, friend.lastName, message.id,
       COALESCE(message.imageFile, message.content), message.creationDate
ORDER BY message.creationDate DESC, message.id ASC
LIMIT 20;
""",
        },
        "interactive-complex-3": {
            "sql": f"""
SELECT p_personid, p_firstname, p_lastname, ct1, ct2, ct1 + ct2 AS totalcount
FROM (
    SELECT k_person2id
    FROM knows
    WHERE k_person1id = {complex3_person}
    UNION
    SELECT k2.k_person2id
    FROM knows k1, knows k2
    WHERE k1.k_person1id = {complex3_person}
      AND k1.k_person2id = k2.k_person1id
      AND k2.k_person2id <> {complex3_person}
) f, person, place p1, place p2, (
    SELECT chn.m_creatorid, ct1, ct2
    FROM (
        SELECT m_creatorid, count(*) AS ct1
        FROM message, place
        WHERE m_locationid = pl_placeid
          AND pl_name = {sql_literal(complex3_country_x)}
          AND m_creationdate >= '{complex3_start:%Y-%m-%d %H:%M:%S}'
          AND m_creationdate < '{complex3_end:%Y-%m-%d %H:%M:%S}'
        GROUP BY m_creatorid
    ) chn, (
        SELECT m_creatorid, count(*) AS ct2
        FROM message, place
        WHERE m_locationid = pl_placeid
          AND pl_name = {sql_literal(complex3_country_y)}
          AND m_creationdate >= '{complex3_start:%Y-%m-%d %H:%M:%S}'
          AND m_creationdate < '{complex3_end:%Y-%m-%d %H:%M:%S}'
        GROUP BY m_creatorid
    ) ind
    WHERE chn.m_creatorid = ind.m_creatorid
) cpc
WHERE f.k_person2id = p_personid
  AND p_placeid = p1.pl_placeid
  AND p1.pl_containerplaceid = p2.pl_placeid
  AND p2.pl_name <> {sql_literal(complex3_country_x)}
  AND p2.pl_name <> {sql_literal(complex3_country_y)}
  AND f.k_person2id = cpc.m_creatorid
ORDER BY totalcount DESC, p_personid ASC
LIMIT 20;
""",
            "gql": f"""
MATCH (source:Person)-[:KNOWS]->{{1,2}}(friend:Person),
      (friend)-[:IS_LOCATED_IN]->(:City)-[:IS_PART_OF]->(home_country:Country),
      (message_x:Message)-[:HAS_CREATOR]->(friend),
      (message_x)-[:IS_LOCATED_IN]->(country_x:Country),
      (message_y:Message)-[:HAS_CREATOR]->(friend),
      (message_y)-[:IS_LOCATED_IN]->(country_y:Country)
WHERE source.id = {complex3_person}
  AND friend.id <> source.id
  AND home_country.name <> {sql_literal(complex3_country_x)}
  AND home_country.name <> {sql_literal(complex3_country_y)}
  AND country_x.name = {sql_literal(complex3_country_x)}
  AND country_y.name = {sql_literal(complex3_country_y)}
  AND message_x.creationDate >= '{complex3_start:%Y-%m-%d %H:%M:%S}'
  AND message_x.creationDate < '{complex3_end:%Y-%m-%d %H:%M:%S}'
  AND message_y.creationDate >= '{complex3_start:%Y-%m-%d %H:%M:%S}'
  AND message_y.creationDate < '{complex3_end:%Y-%m-%d %H:%M:%S}'
RETURN friend.id, friend.firstName, friend.lastName,
       COUNT(DISTINCT message_x.id) AS ct1,
       COUNT(DISTINCT message_y.id) AS ct2,
       COUNT(DISTINCT message_x.id) + COUNT(DISTINCT message_y.id) AS totalcount
GROUP BY friend
ORDER BY totalcount DESC, friend.id ASC
LIMIT 20;
""",
        },
        "interactive-complex-4": {
            "sql": f"""
SELECT t_name, count(*) AS postCount
FROM tag, message, message_tag recent, knows
WHERE m_messageid = mt_messageid
  AND mt_tagid = t_tagid
  AND m_creatorid = k_person2id
  AND m_c_replyof IS NULL
  AND k_person1id = {complex4_person}
  AND m_creationdate >= '{complex4_start:%Y-%m-%d %H:%M:%S}'
  AND m_creationdate < '{complex4_end:%Y-%m-%d %H:%M:%S}'
  AND NOT EXISTS (
      SELECT 1
      FROM message old_message, message_tag old_tag, knows old_knows
      WHERE old_knows.k_person1id = {complex4_person}
        AND old_knows.k_person2id = old_message.m_creatorid
        AND old_message.m_c_replyof IS NULL
        AND old_tag.mt_messageid = old_message.m_messageid
        AND old_message.m_creationdate < '{complex4_start:%Y-%m-%d %H:%M:%S}'
        AND old_tag.mt_tagid = recent.mt_tagid
  )
GROUP BY t_name
ORDER BY postCount DESC, t_name ASC
LIMIT 10;
""",
            "gql": f"""
MATCH (source:Person)-[:KNOWS]->(friend:Person),
      (recent:Post)-[:HAS_CREATOR]->(friend),
      (recent)-[:HAS_TAG]->(tag:Tag)
WHERE source.id = {complex4_person}
  AND recent.creationDate >= '{complex4_start:%Y-%m-%d %H:%M:%S}'
  AND recent.creationDate < '{complex4_end:%Y-%m-%d %H:%M:%S}'
OPTIONAL MATCH (source)-[:KNOWS]->(old_friend:Person),
               (old_post:Post)-[:HAS_CREATOR]->(old_friend),
               (old_post)-[:HAS_TAG]->(tag)
WHERE old_post.creationDate < '{complex4_start:%Y-%m-%d %H:%M:%S}'
LET old_post_id = old_post.id
FILTER old_post_id IS NULL
RETURN tag.name, COUNT(DISTINCT recent.id) AS postCount
GROUP BY tag
ORDER BY postCount DESC, tag.name ASC
LIMIT 10;
""",
        },
        "interactive-complex-5": {
            "sql": f"""
SELECT f_title, count(m_messageid) AS postCount
FROM (
    SELECT f_title, f_forumid, f.k_person2id
    FROM forum, forum_person, (
        SELECT k_person2id
        FROM knows
        WHERE k_person1id = {complex5_person}
        UNION
        SELECT k2.k_person2id
        FROM knows k1, knows k2
        WHERE k1.k_person1id = {complex5_person}
          AND k1.k_person2id = k2.k_person1id
          AND k2.k_person2id <> {complex5_person}
    ) f
    WHERE f_forumid = fp_forumid
      AND fp_personid = f.k_person2id
      AND fp_joindate >= '{complex5_date:%Y-%m-%d %H:%M:%S}'
) tmp
LEFT JOIN message
  ON tmp.f_forumid = m_ps_forumid
 AND m_creatorid = tmp.k_person2id
GROUP BY f_forumid, f_title
ORDER BY postCount DESC, f_forumid ASC
LIMIT 20;
""",
            "gql": f"""
MATCH (source:Person)-[:KNOWS]->{{1,2}}(friend:Person),
      (forum:Forum)-[membership:HAS_MEMBER]->(friend)
WHERE source.id = {complex5_person}
  AND friend.id <> source.id
  AND membership.joinDate >= '{complex5_date:%Y-%m-%d %H:%M:%S}'
OPTIONAL MATCH (forum)-[:CONTAINER_OF]->(post:Post)-[:HAS_CREATOR]->(friend)
RETURN forum.title, COUNT(DISTINCT post.id) AS postCount
GROUP BY forum
ORDER BY postCount DESC, forum.id ASC
LIMIT 20;
""",
        },
        "interactive-complex-6": {
            "sql": f"""
SELECT t_name, count(*) AS postCount
FROM tag, message_tag, message, (
    SELECT k_person2id
    FROM knows
    WHERE k_person1id = {complex6_person}
    UNION
    SELECT k2.k_person2id
    FROM knows k1, knows k2
    WHERE k1.k_person1id = {complex6_person}
      AND k1.k_person2id = k2.k_person1id
      AND k2.k_person2id <> {complex6_person}
) f
WHERE m_creatorid = f.k_person2id
  AND m_c_replyof IS NULL
  AND m_messageid = mt_messageid
  AND mt_tagid = t_tagid
  AND t_name <> {sql_literal(complex6_tag)}
  AND EXISTS (
      SELECT 1
      FROM tag required_tag, message_tag required_message_tag
      WHERE required_message_tag.mt_messageid = m_messageid
        AND required_message_tag.mt_tagid = required_tag.t_tagid
        AND required_tag.t_name = {sql_literal(complex6_tag)}
  )
GROUP BY t_name
ORDER BY postCount DESC, t_name ASC
LIMIT 10;
""",
            "gql": f"""
MATCH (source:Person)-[:KNOWS]->{{1,2}}(friend:Person),
      (post:Post)-[:HAS_CREATOR]->(friend),
      (post)-[:HAS_TAG]->(tag:Tag),
      (post)-[:HAS_TAG]->(required_tag:Tag)
WHERE source.id = {complex6_person}
  AND friend.id <> source.id
  AND tag.name <> {sql_literal(complex6_tag)}
  AND required_tag.name = {sql_literal(complex6_tag)}
RETURN tag.name, COUNT(DISTINCT post.id) AS postCount
GROUP BY tag
ORDER BY postCount DESC, tag.name ASC
LIMIT 10;
""",
            "single_run": True,
        },
        "interactive-complex-8": {
            "sql": f"""
SELECT p1.m_creatorid, p_firstname, p_lastname, p1.m_creationdate,
       p1.m_messageid, p1.m_content
FROM message p1, message p2, person
WHERE p1.m_c_replyof = p2.m_messageid
  AND p2.m_creatorid = {complex8_person}
  AND p_personid = p1.m_creatorid
ORDER BY p1.m_creationdate DESC, p1.m_messageid ASC
LIMIT 20;
""",
            "gql": f"""
MATCH (source:Person) WHERE source.id = {complex8_person}
MATCH (parent:Message)-[:HAS_CREATOR]->(source)
MATCH (reply:Comment)-[:REPLY_OF]->(parent)
MATCH (reply)-[:HAS_CREATOR]->(author:Person)
RETURN author.id, author.firstName, author.lastName,
       reply.creationDate, reply.id, reply.content
ORDER BY reply.creationDate DESC, reply.id ASC
LIMIT 20;
""",
            "single_run": True,
        },
        "interactive-complex-9": {
            "sql": f"""
SELECT p_personid, p_firstname, p_lastname,
       m_messageid, COALESCE(m_ps_imagefile, m_content), m_creationdate
FROM (
    SELECT k_person2id
    FROM knows
    WHERE k_person1id = {complex9_person}
    UNION
    SELECT k2.k_person2id
    FROM knows k1, knows k2
    WHERE k1.k_person1id = {complex9_person}
      AND k1.k_person2id = k2.k_person1id
      AND k2.k_person2id <> {complex9_person}
) f, person, message
WHERE p_personid = m_creatorid
  AND p_personid = f.k_person2id
  AND m_creationdate < '{complex9_date:%Y-%m-%d %H:%M:%S}'
ORDER BY m_creationdate DESC, m_messageid ASC
LIMIT 20;
""",
            "gql": f"""
MATCH (source:Person)-[:KNOWS]->{{1,2}}(friend:Person),
      (message:Message)-[:HAS_CREATOR]->(friend)
WHERE source.id = {complex9_person}
  AND friend.id <> source.id
  AND message.creationDate < '{complex9_date:%Y-%m-%d %H:%M:%S}'
RETURN DISTINCT friend.id, friend.firstName, friend.lastName, message.id,
       COALESCE(message.imageFile, message.content), message.creationDate
ORDER BY message.creationDate DESC, message.id ASC
LIMIT 20;
""",
        },
        "interactive-complex-11": {
            "sql": f"""
SELECT p_personid, p_firstname, p_lastname, o_name, pc_workfrom
FROM (
    SELECT k_person2id
    FROM knows
    WHERE k_person1id = {complex11_person}
    UNION
    SELECT k2.k_person2id
    FROM knows k1, knows k2
    WHERE k1.k_person1id = {complex11_person}
      AND k1.k_person2id = k2.k_person1id
      AND k2.k_person2id <> {complex11_person}
) f, person, person_company, organisation, place
WHERE p_personid = f.k_person2id
  AND p_personid = pc_personid
  AND pc_organisationid = o_organisationid
  AND o_placeid = pl_placeid
  AND pl_name = {sql_literal(complex11_country)}
  AND pc_workfrom < {complex11_year}
ORDER BY pc_workfrom ASC, p_personid ASC, o_name DESC
LIMIT 10;
""",
            "gql": f"""
MATCH (source:Person)-[:KNOWS]->{{1,2}}(friend:Person),
      (friend)-[work:WORK_AT]->(organisation:Organisation)-[:IS_LOCATED_IN]->(country:Country)
WHERE source.id = {complex11_person}
  AND friend.id <> source.id
  AND work.workFrom < {complex11_year}
  AND country.name = {sql_literal(complex11_country)}
RETURN DISTINCT friend.id, friend.firstName, friend.lastName,
       organisation.name, work.workFrom
ORDER BY work.workFrom ASC, friend.id ASC, organisation.name DESC
LIMIT 10;
""",
        },
        "interactive-complex-13": {
            "sql": f"""
WITH RECURSIVE search_graph(link, level, path) AS (
    SELECT {complex13_person1}::INT64, 0, [{complex13_person1}::INT64]
    UNION ALL
    (
        WITH sg(link, level) AS (SELECT * FROM search_graph)
        SELECT DISTINCT k_person2id, x.level + 1,
               array_append(path, k_person2id)
        FROM knows, sg x
        WHERE x.link = k_person1id
          AND NOT EXISTS (
              SELECT * FROM sg y
              WHERE y.link = {complex13_person2}::INT64
          )
          AND NOT EXISTS (
              SELECT * FROM sg y
              WHERE y.link = k_person2id
          )
    )
)
SELECT max(level) AS shortestPathLength
FROM (
    SELECT level FROM search_graph WHERE link = {complex13_person2}
    UNION SELECT -1
) result;
""",
            "gql": f"""
MATCH (source:Person), (target:Person)
WHERE source.id = {complex13_person1}
  AND target.id = {complex13_person2}
CALL algo.shortest_path_length(
    'snb_interactive',
    element_id(source),
    element_id(target),
    'Person',
    'KNOWS'
)
YIELD distance
RETURN distance;
""",
        },
    }
    unsupported = {
        "interactive-complex-1": "requires shortest-distance selection plus nested collection projections",
        "interactive-complex-7": "requires a grouped max subquery, temporal arithmetic, and conditional projection",
        "interactive-complex-10": "requires correlated aggregate subqueries and date extraction",
        "interactive-complex-12": "requires tag-class recursion and distinct list aggregation",
        "interactive-complex-14": "requires all shortest paths, edge weighting, and path-list aggregation",
    }
    if selected_queries:
        known = set(cases) | set(unsupported)
        unknown = sorted(selected_queries - known)
        if unknown:
            raise RuntimeError(f"unknown benchmark queries: {', '.join(unknown)}")
        cases = {
            name: case for name, case in cases.items()
            if name in selected_queries
        }
        unsupported = {
            name: reason for name, reason in unsupported.items()
            if name in selected_queries
        }

    results: dict[str, Any] = {}
    for name, case in cases.items():
        print(f"[{name}] validating SQL and GQL rows", flush=True)
        expected, _ = query_rows(
            cli,
            relational_database,
            case["sql"],
            False,
            timeout,
            threads,
            memory_limit,
        )
        actual, first_profile = query_rows(
            cli,
            graph_database,
            case["gql"],
            True,
            timeout,
            threads,
            memory_limit,
        )
        validated = actual == expected
        entry: dict[str, Any] = {
            "status": "passed" if validated else "row_mismatch",
            "validation": "exact ordered CSV rows",
            "result_rows": len(actual),
            "expected_rows": len(expected),
            "result_sha256": hashlib.sha256(
                json.dumps(actual, separators=(",", ":")).encode()
            ).hexdigest(),
            "expected_sha256": hashlib.sha256(
                json.dumps(expected, separators=(",", ":")).encode()
            ).hexdigest(),
            "gql": case["gql"].strip(),
        }
        if first_profile:
            entry["first_run"] = {
                "latency_seconds": first_profile["latency"],
                "cpu_seconds": first_profile["cpu_time"],
                "cumulative_rows_scanned": first_profile[
                    "cumulative_rows_scanned"
                ],
            }
        if not case.get("single_run"):
            entry["warm_benchmark"] = benchmark_repeated(
                cli,
                graph_database,
                case["gql"],
                threads,
                memory_limit,
                warmups,
                runs,
                timeout,
            )
        results[name] = entry
        latency = entry.get("warm_benchmark", {}).get(
            "latency_seconds", {}
        ).get("median", entry.get("first_run", {}).get("latency_seconds"))
        print(
            f"[{name}] {entry['status']}, {len(actual)} rows, "
            f"{latency:.6f}s" if latency is not None else
            f"[{name}] {entry['status']}, {len(actual)} rows",
            flush=True,
        )
    return results, unsupported


def source_metadata() -> dict[str, Any]:
    root = Path(__file__).resolve().parents[2]
    return {
        "commit": subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=root,
            text=True,
            capture_output=True,
            check=True,
        ).stdout.strip(),
        "dirty": bool(
            subprocess.run(
                ["git", "status", "--porcelain", "--untracked-files=no"],
                cwd=root,
                text=True,
                capture_output=True,
                check=True,
            ).stdout.strip()
        ),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", type=Path, default=Path("build/release/duckdb"))
    parser.add_argument(
        "--source-database",
        type=Path,
        default=Path("build/benchmarks/snb10/snb10-relational.duckdb"),
    )
    parser.add_argument(
        "--vertices",
        type=Path,
        default=Path("build/benchmarks/snb10/graph/snb10-vertices.parquet"),
    )
    parser.add_argument(
        "--edges",
        type=Path,
        default=Path("build/benchmarks/snb10/graph/snb10-edges.parquet"),
    )
    parser.add_argument(
        "--graph-database",
        type=Path,
        default=Path("build/benchmarks/snb10/snb10-gql.duckdb"),
    )
    parser.add_argument(
        "--relational-results",
        type=Path,
        default=Path("build/benchmarks/snb10/interactive-results.json"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("build/benchmarks/snb10/gql-interactive-results.json"),
    )
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--memory-limit", default="8GB")
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--query-timeout", type=float, default=180.0)
    parser.add_argument(
        "--query",
        action="append",
        dest="queries",
        help="run only this query name; repeat for multiple queries",
    )
    parser.add_argument("--export-only", action="store_true")
    parser.add_argument("--no-queries", action="store_true")
    args = parser.parse_args()

    cli = args.duckdb.resolve()
    source_database = args.source_database.resolve()
    vertices = args.vertices.resolve()
    edges = args.edges.resolve()
    graph_database = args.graph_database.resolve()
    relational_results = args.relational_results.resolve()
    output = args.output.resolve()
    for path in (cli, source_database, relational_results):
        if not path.exists():
            raise SystemExit(f"missing required path: {path}")
    if args.warmups < 0 or args.runs < 1 or args.query_timeout <= 0:
        raise SystemExit("warmups must be non-negative; runs and timeout must be positive")

    started = time.perf_counter()
    exported = ensure_parquet(
        cli,
        source_database,
        vertices,
        edges,
        args.threads,
        args.memory_limit,
    )
    graph: dict[str, Any] = {}
    queries: dict[str, Any] = {}
    unsupported: dict[str, Any] = {}
    if not args.export_only:
        graph = ensure_graph(
            cli,
            graph_database,
            vertices,
            edges,
            args.threads,
            args.memory_limit,
        )
        if not args.no_queries:
            queries, unsupported = benchmark_cases(
                cli,
                source_database,
                graph_database,
                relational_results,
                args.threads,
                args.memory_limit,
                args.warmups,
                args.runs,
                args.query_timeout,
                set(args.queries) if args.queries else None,
            )
    payload = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "benchmark_kind": (
            "LDBC SNB Interactive v1 DuckGQL engineering run; "
            "not an official driver score"
        ),
        "source": source_metadata(),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "configuration": {
            "threads": args.threads,
            "memory_limit": args.memory_limit,
            "warmups": args.warmups,
            "runs": args.runs,
            "query_timeout_seconds": args.query_timeout,
            "build_csr_before_gql_query": True,
        },
        "source_database": str(source_database),
        "parquet": exported,
        "graph_database": str(graph_database),
        "graph_database_bytes": (
            graph_database.stat().st_size if graph_database.exists() else 0
        ),
        "graph": graph,
        "queries": queries,
        "unsupported": unsupported,
        "total_wall_seconds": time.perf_counter() - started,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, indent=2) + "\n")
    print(json.dumps({"output": str(output), "graph": graph}, indent=2))


if __name__ == "__main__":
    main()
