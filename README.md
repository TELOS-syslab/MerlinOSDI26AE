# Merlin OSDI'26 Artifact Evaluation Guide

This artifact accompanies the OSDI'26 paper, "Merlin: An Efficient Adaptive
Cache Eviction Algorithm via Fine-Grained Characterization."

The artifact contains the Merlin implementation, the modified libCacheSim and
CacheLib evaluation harnesses, scripts for reproducing the paper figures, and
precomputed result files for the large experiments. The full trace-driven
evaluation is intentionally optional because it requires very large public trace
datasets and substantial compute time.

## Artifact Claims

This artifact supports the main evaluation claims in the paper:

- Merlin improves hit rate and byte hit rate over existing cache eviction
  algorithms across diverse traces.
- Merlin improves the relative hit ratio compared with the dominant algorithm
  on representative traces.
- Merlin achieves competitive throughput in the CacheLib-based implementation.
- Merlin reduces flash write amplification while preserving hit rate in the
  CloudPhysics flash-cache experiment.

The repository includes precomputed results under `data/` so that reviewers can
quickly regenerate the figures. The scripts under `scripts/` and
`CacheLib/mybench/` document how the corresponding results were produced.

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

## Repository Layout

- `README.md`: this artifact evaluation guide.
- `scripts/`: installation, evaluation, post-processing, and plotting scripts.
- `data/`: precomputed data used by the plotting scripts.
- `data/HR/`: hit-rate and byte-hit-rate summaries for Figure 11 and Figure 12.
- `data/RHR/`: relative-hit-ratio summaries for Figure 13.
- `data/throughput/`: throughput results for Figure 14.
- `data/flash/`: flash-cache results for Figure 15.
- `libCacheSim/`: the modified libCacheSim code used for simulation.
- `CacheLib/`: the modified CacheLib code used for throughput evaluation.
- `CacheLib/mybench/`: CacheLib microbenchmark and trace-generation scripts.

## Getting Started Instructions

These instructions are intended to check the basic functionality of the artifact
within a short time frame. They regenerate figures from the precomputed result
files and do not require downloading the full trace datasets.

Estimated runtime for this section: about 10-30 minutes end-to-end on a common
desktop/server CPU.

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

# Figure 17: precision of access pattern identification.
python3 scripts/plot/precision.py data/precision/fiu.dat -o fiu.pdf
python3 scripts/plot/precision.py data/precision/twitter.dat -o twitter.pdf
```

The expected output files are:

- `hit_rate.pdf`
- `byte_hit_rate.pdf`
- `relative_hit_ratio.pdf`
- `throughput.pdf`
- `flash.pdf`

## Detailed Instructions

This section describes how to rebuild the artifact and reproduce the results in
more detail. The full experiments are optional for artifact evaluation because
of their runtime and storage cost.

### Build Dependencies and Binaries

Estimated runtime and resources:

- Runtime: about [TO_FILL_MIN_INSTALL] minutes
- CPU: moderate to high during compilation
- Memory: recommend >= [TO_FILL_RAM_INSTALL]
- Additional disk: about [TO_FILL_DISK_INSTALL] (build artifacts + Docker image)

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

Estimated resource cost:

- Download size (full set): about 2 TB
- Download time: about a few hors to a few days (depends on network bandwidth)

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
Estimated runtime and resources:
- Aabout 1 million CPU-hours in aggregate
- Memory: recommend >= 1 TB for parallel execution
- Disk: traces up to about 2 TB + outputs about 500 MB

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
Estimated runtime and resources:
- CPU: high
- Memory: [TO_FILL_RAM_FIG14]
- Disk: [TO_FILL_DISK_FIG14_RERUN]

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
created by `scripts/install.sh`. The benchmark scripts in `CacheLib/mybench/`
show the exact algorithm, cache-size, hash-power, and thread-count settings used
for the throughput experiment.

```bash
bash scripts/throughput.sh woback
bash scripts/throughput.sh wback
```

### Figure 15: Flash-Cache Hit Rate and Write Bytes
Estimated runtime and resources:
- CPU: high
- Memory: [TO_FILL_RAM_FIG14]
- Disk: [TO_FILL_DISK_FIG14_RERUN]

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

### Figure 17: Parameter-Tracking

The parameter-tracking build is created under `libCacheSim/_build2/`. Example
commands for inspecting Merlin and baselines on individual traces are:

```bash
./libCacheSim/_build2/bin/cachesim \
  ./CacheTrace/twitter/cluster8.oracleGeneral.zst \
  oracleGeneral merlin 0.1 --ignore-obj-size=true

./libCacheSim/_build2/bin/cachesim \
  ./CacheTrace/twitter/cluster8.oracleGeneral.zst \
  oracleGeneral cacheus 0.1 --ignore-obj-size=true

./libCacheSim/_build2/bin/cachesim \
  ./CacheTrace/twitter/cluster8.oracleGeneral.zst \
  oracleGeneral arc 0.1 --ignore-obj-size=true

./libCacheSim/_build2/bin/cachesim \
  ./CacheTrace/fiu/fiu_webmail.cs.fiu.edu-110108-113008.oracleGeneral.zst \
  oracleGeneral merlin 0.2 --ignore-obj-size=true

./libCacheSim/_build2/bin/cachesim \
  ./CacheTrace/fiu/fiu_webmail.cs.fiu.edu-110108-113008.oracleGeneral.zst \
  oracleGeneral cacheus 0.2 --ignore-obj-size=true

./libCacheSim/_build2/bin/cachesim \
  ./CacheTrace/fiu/fiu_webmail.cs.fiu.edu-110108-113008.oracleGeneral.zst \
  oracleGeneral arc 0.2 --ignore-obj-size=true
```

Or you can run the script
```bash
bash scripts/precision.sh
python3 scripts/plot/precision.py data/precision/fiu.dat -o fiu.pdf
python3 scripts/plot/precision.py data/precision/twitter.dat -o twitter.pdf
```


## Notes for Reviewers

- The quick-start path regenerates all provided figures from bundled data and is
  the recommended AE baseline due to low resource cost.
- The full trace-driven evaluation is reproducible but very expensive in CPU
  time, memory, and storage; we recommend spot-checking representative traces
  unless sufficient compute is available.
- The scripts are resumable in the sense that they inspect existing output files
  and skip completed policy results when possible.
- No private datasets are required. The large traces are publicly available from
  the URLs listed above.
- The artifact does not intentionally perform destructive operations. The
  installation scripts do use `sudo apt`, `sudo make install`, and Docker.
- The repository does not include the full trace corpus. This keeps the artifact
  package small and relies on the public dataset archive listed below.
