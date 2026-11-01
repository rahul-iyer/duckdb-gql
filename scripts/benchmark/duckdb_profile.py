"""Keep benchmark report metrics consistent across DuckDB profiler versions."""

from typing import Any


def normalize_profile(value: dict[str, Any]) -> dict[str, Any]:
    """Expose the existing report keys for DuckDB 2.0's grouped JSON metrics.

    Preserve the original grouped data for inspection and accept older profiles
    unchanged so A/B runs can compare binaries from both DuckDB versions.
    """
    query = value.get("query")
    if not isinstance(query, dict) or "sql" not in query:
        return value
    return {
        **value,
        "query_name": query["sql"],
        "latency": query["total_time"],
        "cpu_time": query["cpu_time"],
        "cumulative_rows_scanned": query["total_rows_scanned"],
        "cumulative_cardinality": query["total_intermediate_rows"],
    }
