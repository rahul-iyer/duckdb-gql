# DuckGQL Graph500 PageRank benchmark

The runner downloads the official Graph500 Kronecker generator, compiles a
streaming CSV adapter, imports each requested scale into DuckGQL, and runs
PageRank to convergence. PageRank automatically builds an outgoing-only CSR
projection without edge IDs, labels, incoming topology, or label postings. The
runner records one cold CSR-plus-PageRank execution followed by one warm
PageRank execution over the cached projection.

Start with one moderate scale:

~~~sh
python3 benchmark/graph500/run_pagerank.py \
  --scales 20 \
  --memory-limit 4GB
~~~

Grow progressively toward one billion edge tuples:

~~~sh
python3 benchmark/graph500/run_pagerank.py \
  --scales 22 23 24 25 26 \
  --memory-limit 4GB
~~~

At edge factor 16, scale 26 contains 1,073,741,824 edge tuples. The runner
estimates native CSR and PageRank allocations before each scale and stops when
they exceed 75% of physical memory. Pass <code>--force</code> only when the
machine has enough memory or swap and you accept the risk.

<code>SET memory_limit</code> constrains DuckDB's buffer manager and
spill-capable query operators. DuckGQL's algorithm-owned CSR and PageRank
vectors are native extension allocations, so the script guards those
separately. The estimate reflects the unfiltered PageRank projection rather
than the full optimizer/inspection CSR created by <code>gql_build_csr</code>.

Results are written to <code>pagerank-results.json</code> under the work
directory (<code>/tmp/duckgql-graph500</code> by default), with per-scale logs
beside each database. Existing CSV files and imported databases are reused
unless <code>--regenerate</code> or <code>--reimport</code> is passed.

The official generator's tuple orientation is treated as directed by DuckGQL
PageRank. The runner does not symmetrize edges and is not an official Graph500
submission.
