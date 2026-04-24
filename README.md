# Merlin OSDI'26 Artifact Evaluation Guide

This artifact accompanies the OSDI'26 paper, "Merlin: An Efficient Adaptive
Cache Eviction Algorithm via Fine-Grained Characterization."

The artifact includes the Merlin implementation, the modified libCacheSim and
CacheLib evaluation harnesses, the plotting scripts, and precomputed result
files for the large experiments. The full trace-driven evaluation is optional
because it needs large public traces and substantial compute time.

## Artifact Claims

This artifact supports the main evaluation claims in the paper:

- Merlin improves hit rate and byte hit rate over existing cache eviction
  algorithms across diverse traces.
- Merlin improves the relative hit ratio compared with the dominant algorithm
  on representative traces.
- Merlin achieves competitive throughput in the CacheLib-based implementation.
- Merlin reduces flash write amplification while preserving hit rate in the
  CloudPhysics flash-cache experiment.

Precomputed results are bundled under `data/` so reviewers can regenerate the
main figures quickly. The scripts under `scripts/` and `CacheLib/mybench/`
document how the corresponding results were produced.

## Hardware and Software Requirements

We recommend an x86-64 machine running Ubuntu 22.04 LTS. We tested the artifact
on an AMD server with 384 hardware threads (two AMD EPYC 9965 processors).

For the quick checks, a normal Linux machine with Python 3 is sufficient. Full
trace-driven reproduction needs much more storage and compute:

- The public traces are approximately 2 TB if downloaded in full.
- Reproducing all Figure 11-13 simulations can take roughly 1 million CPU hours.
- The simulator runs experiments in parallel, but each job can require
  significant memory. If memory is limited, reduce parallelism or run individual
  experiments manually.

The main software dependencies are Python 3, `pip`, `pandas`, `numpy`,
`matplotlib`, `psutil`, Docker, CMake, libCacheSim dependencies, and CacheLib
dependencies. The helper scripts in `scripts/install_dependency.sh` and
`scripts/install.sh` install and build the required components on Ubuntu 22.04.

## Getting Started Instructions

These instructions check the basic functionality of the artifact within a short
time frame. They regenerate figures from the bundled results and do not require
downloading the full trace datasets.

Estimated runtime: about 10-30 minutes end-to-end on a common desktop/server
CPU.

Clone the repository with submodules:

```bash
git clone --recursive https://github.com/TELOS-syslab/MerlinOSDI26AE.git
cd MerlinOSDI26AE
```

Install the Python packages needed by the plotting scripts:

```bash
python3 -m pip install pandas numpy matplotlib psutil
```

Regenerate the figures from the provided results:

```bash
# Figure 11: hit rate
python3 scripts/plot/hit_rate.py

# Figure 12: byte hit rate
python3 scripts/plot/byte_hit_rate.py

# Figure 13: relative hit ratio
python3 scripts/plot/relative_hit_ratio.py

# Figure 14: throughput
python3 scripts/plot/throughput.py

# Figure 15: flash-cache hit rate and normalized write bytes
python3 scripts/plot/flash.py

# Figure 16: precomputed sensitivity inputs are bundled in data/sensitivity/
# Figure 17: precision of access pattern identification
python3 scripts/plot/precision.py data/precision/fiu.dat -o fiu.pdf
python3 scripts/plot/precision.py data/precision/twitter.dat -o twitter.pdf
```

The expected output files are:

- `hit_rate.pdf`
- `byte_hit_rate.pdf`
- `relative_hit_ratio.pdf`
- `throughput.pdf`
- `flash.pdf`
- `fiu.pdf`
- `twitter.pdf`

## Detailed Instructions

This section describes how to rebuild the artifact and reproduce the results in
more detail. The full experiments are optional because they are expensive in
runtime, memory, and storage.

### Build Dependencies and Binaries

Estimated cost: about 10 minutes, moderate-to-high CPU load, recommend
`>=128GB` RAM, and about `21GB` of additional disk for build artifacts and the
Docker image.

On Ubuntu 22.04, install system dependencies and build libCacheSim and CacheLib:

```bash
bash ./scripts/install_dependency.sh
bash ./scripts/install.sh
```

`scripts/install_dependency.sh` installs the system packages and builds zstd,
XGBoost, and LightGBM. `scripts/install.sh` builds two libCacheSim variants:

- `libCacheSim/_build/`: normal build for the main simulations.
- `libCacheSim/_build2/`: build with `TRACK_PARAMETERS` enabled for parameter
  tracking experiments.

The same script also builds the Docker image `cachelib-ae` and compiles the
CacheLib microbenchmark in `CacheLib/mybench/`.

### Trace Dataset Layout

Estimated resource cost: about `2TB` download size; download time depends on
network bandwidth and can range from a few hours to a few days.

The Figure 11-13 simulations use public traces from the cache-dataset archive:

<https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/cacheDatasets/>

Place the downloaded traces under `CacheTrace/` or pass another directory with
`--input_dir`. The expected layout is:

```text
CacheTrace/
  alibabaBlock/
  cloudphysics/
  fiu/
  metaCDN/
  metaKV/
  msr/
  systor/
  tencentBlock/
  tencentPhoto/
  twitter/
  wikimedia/
```

The full dataset is large. For spot checks, download only the traces needed for
the experiment you want to inspect.

### Figure 11-13: Hit Rate, Byte Hit Rate, and Relative Hit Ratio

Recommended AE path: use the bundled results and plotting scripts.

Estimated cost for a full rerun: about `1M CPU-hours` in aggregate, `>=1TB`
RAM for aggressive parallel execution, and about `2TB` of trace storage plus
small outputs.

The precomputed results are already included in:

- `data/HR/withoutobjsize/`
- `data/HR/withobjsize/`
- `data/RHR/withoutobjsize/`
- `data/RHR/withobjsize/`

To regenerate the figures from these results, run:

```bash
python3 scripts/plot/hit_rate.py
python3 scripts/plot/byte_hit_rate.py
python3 scripts/plot/relative_hit_ratio.py
```

To rerun the full simulation, use:

```bash
# Object size ignored: used for the hit-rate figure.
python3 scripts/evaluation.py \
  --root_dir ./libCacheSim/_build \
  --input_dir ./CacheTrace \
  --output_dir ./results/eval_ignore_obj \
  --ignore_obj

# Object size considered: used for the byte-hit-rate figure.
python3 scripts/evaluation.py \
  --root_dir ./libCacheSim/_build \
  --input_dir ./CacheTrace \
  --output_dir ./results/eval_with_obj
```

Post-process the simulation outputs:

```bash
python3 scripts/readeval.py \
  --input_dir ./results/eval_ignore_obj \
  --output_dir ./dataresult/withoutobjsize \
  --normalize_policy

python3 scripts/readeval.py \
  --input_dir ./results/eval_with_obj \
  --output_dir ./dataresult/withobjsize \
  --normalize_policy

python3 scripts/getHR.py \
  --input_dir ./dataresult/withoutobjsize \
  --output_dir ./data/HR/withoutobjsize

python3 scripts/getHR.py \
  --input_dir ./dataresult/withobjsize \
  --output_dir ./data/HR/withobjsize

python3 scripts/getRHRcdf.py \
  --input_dir ./dataresult/withoutobjsize \
  --output_dir ./data/RHR/withoutobjsize

python3 scripts/getRHRcdf.py \
  --input_dir ./dataresult/withobjsize \
  --output_dir ./data/RHR/withobjsize
```

### Figure 14: Throughput

Recommended AE path: use the bundled results and `scripts/plot/throughput.py`.

Estimated cost for the optional rerun: high CPU load, recommend `>=32GB` RAM,
and about `6GB` disk for the synthetic trace.

The precomputed throughput results are in:

- `data/throughput/wback/`
- `data/throughput/woback/`

Regenerate the figure:

```bash
python3 scripts/plot/throughput.py
```

To reproduce the throughput data, first generate the synthetic mixed trace:

```bash
python3 ./CacheLib/mybench/data_genmix.py \
  -m 1000000 \
  -n 100000000 \
  --bin-output ./mix.oracleGeneral.bin
```

Then run the CacheLib microbenchmark inside the `cachelib-ae` Docker image
created by `scripts/install.sh`. The helper script `scripts/throughput.sh`
records the algorithm, cache-size, hash-power, and thread-count settings used
for this experiment.

```bash
bash scripts/throughput.sh woback
bash scripts/throughput.sh wback
```

### Figure 15: Flash-Cache Hit Rate and Write Bytes

Recommended AE path: use the bundled results and `scripts/plot/flash.py`.

Estimated cost for the optional rerun: high CPU load, recommend `>=32GB` RAM,
and about `8.5GB` disk for the CloudPhysics traces.

The precomputed flash-cache results are in `data/flash/`. Regenerate the figure:

```bash
python3 scripts/plot/flash.py
```

To reproduce the raw flash-cache measurements, download the CloudPhysics traces:

<https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/cacheDatasets/cloudphysics/>

Place the traces under:

```text
CacheTrace/cloudphysics/
```

Then run:

```bash
MAX_JOBS=2 bash scripts/flash.sh
```

`MAX_JOBS` controls the number of concurrent flash-cache jobs. Increase it only
if the machine has enough CPU and memory. The script writes raw outputs under
`data/flash/<cache-size>/`. Use `scripts/process_flash.py` to summarize raw
outputs into the `data/flash/*.txt` files consumed by `scripts/plot/flash.py`.

### Figure 16: Merlin Sensitivity Evaluation

Recommended AE path: use the bundled processed inputs in `data/sensitivity/`.

Estimated cost for the optional rerun: very high CPU load, recommend
`>=128GB` RAM, and about `2TB` of trace storage plus outputs.

Figure 16 studies Merlin's sensitivity to the `filter-size-ratio`,
`staging-size-ratio`, and `ghost-size-ratio` parameters. The precomputed
`.dat` files already in `data/sensitivity/` contain the percentile summaries
used by the paper.

To reproduce the raw sensitivity evaluation, run Merlin-only simulations on the
same trace corpus used for Figure 11-13:

```bash
python3 scripts/sensitivity.py \
  --root_dir ./libCacheSim/_build \
  --input_dir ./CacheTrace \
  --output_dir ./results/sensitivity \
  --ignore_obj
```

The script writes raw simulator outputs under `./results/sensitivity/`. Convert
them into cache-ratio CSVs with `scripts/readeval.py`:

```bash
python3 scripts/readeval.py \
  --input_dir ./results/sensitivity \
  --output_dir ./dataresult/sensitivity
```

Do not use `--normalize_policy` here, because the Merlin parameter suffixes are
needed for Figure 16 and would otherwise be removed during preprocessing.

Then merge the Merlin sensitivity results with the `LRU` baseline from the main
`withoutobjsize` results and generate the Figure 16 `.dat` files:

```bash
python3 scripts/getSen.py \
  --input_dir ./dataresult/sensitivity \
  --baseline_dir ./dataresult/withoutobjsize \
  --output_dir ./data/sensitivity
```

`scripts/getSen.py` aggregates the selected datasets, computes relative hit
ratio against `LRU`, and emits one `.dat` file per cache-size and ghost-size
slice.

### Figure 17: Access-Pattern Precision

Recommended AE path: use the bundled `data/precision/*.dat` files and
`scripts/plot/precision.py`.

Estimated cost for the optional rerun: high CPU load, recommend `>=128GB` RAM,
and the same trace storage needed for the precision traces.

The parameter-tracking build is created under `libCacheSim/_build2/`. Prepare
the Twitter and FIU traces listed below, then run:

```bash
bash scripts/precision.sh
python3 scripts/plot/precision.py data/precision/fiu.dat -o fiu.pdf
python3 scripts/plot/precision.py data/precision/twitter.dat -o twitter.pdf
```


## Notes for Reviewers

- Use the Getting Started path for the lowest-cost AE check.
- Full reproduction is available but expensive in CPU time, memory, and
  storage.
- The scripts are resumable and skip completed results when possible.
- No private datasets are required; all large traces are publicly available.
- The installation scripts use `sudo apt`, `sudo make install`, and Docker.
