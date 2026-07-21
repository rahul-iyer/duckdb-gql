#!/usr/bin/env python3
"""Compatibility entry point for the native COPY GRAPH load benchmark.

The former implementation compared manual table registration and the EAV
loader. Those backends no longer exist; the maintained benchmark is
``benchmark_copy_graph.py``.
"""

from benchmark_copy_graph import main


if __name__ == "__main__":
    main()
