# Merlin OSDI'26 Artifact Evaluation Guide

This artifact accompanies the OSDI'26 paper, "Merlin: An Efficient Adaptive
Cache Eviction Algorithm via Fine-Grained Characterization."

The artifact includes the Merlin implementation, the modified libCacheSim and
CacheLib evaluation harnesses, the plotting scripts, and precomputed result
files for the large experiments. The full trace-driven evaluation is optional
because it requires large public datasets and substantial compute time.

## AE Workflow Options

Because the datasets are large and full reruns are expensive, we provide three
evaluation workflows for artifact evaluation:

1. Fast path (about 30 minutes): use the preprocessed results bundled in
  `data/` to directly generate figures.
  Command reference: follow [Getting Started Instructions](README.md#getting-started-instructions),
  especially the figure-generation commands under "Regenerate the figures from
  the provided results."
2. Reduced full-run path (without Twitter dataset): to shorten AE time, run all
  datasets except Twitter. The expected wall-clock time is about 7 days on a
  384-core server. We can provide access to a 384-core server for this path.
  Command reference: follow [Detailed Instructions](README.md#detailed-instructions),
  but use an input directory that excludes the Twitter dataset.
3. Complete full-run path (all datasets): run all datasets, including Twitter.
  The expected wall-clock time is about 30 days. Use this path only if time
  permits.
  Command reference: follow all commands in [Detailed Instructions](README.md#detailed-instructions) and the full datasets.

## Artifact Claims

This artifact supports the main evaluation claims in the paper:

- Merlin improves hit-rate and byte-hit-rate metrics over existing cache
  eviction algorithms across diverse workloads ([Figure 11-13](README.md#figure-11-13-hit-rate-byte-hit-rate-and-relative-hit-ratio)).
- Merlin is adaptive and comparable to the dominant algorithms on
  representative datasets ([Figure 11-13](README.md#figure-11-13-hit-rate-byte-hit-rate-and-relative-hit-ratio)).
- Merlin achieves competitive throughput in the CacheLib-based implementation
  ([Figure 14](README.md#figure-14-throughput)).
- Merlin reduces flash write amplification while preserving hit rate in the flash-cache experiment ([Figure 15](README.md#figure-15-flash-cache-hit-rate-and-write-bytes)).
- Merlin remains robust under sensitivity analysis ([Figure 16](README.md#figure-16-merlin-sensitivity-evaluation)).
- Merlin shows a favorable access-pattern identification precision compared with
  baseline mechanisms ([Figure 17](README.md#figure-17-access-pattern-precision)).

Precomputed results are bundled under `data/` so reviewers can regenerate the
main figures quickly (Figures 11-17). The scripts under `scripts/` document how
the corresponding results were produced.

## Hardware and Software Requirements

We recommend an x86-64 machine running Ubuntu 22.04 LTS. We evaluated the artifact
on an AMD server with 384 hardware threads (two AMD EPYC 9965 processors).

For the quick checks, a normal Linux machine with Python 3 is sufficient. Full
trace-driven reproduction needs much more storage and compute:

- The public datasets are approximately 2 TB if downloaded in full.
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
downloading the full datasets.

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

# Figure 16: Merlin's sensitivity to queue size
python3 scripts/plot/sensitivity.py

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
- `sensitivity.pdf`
- `fiu.pdf`
- `twitter.pdf`

## Detailed Instructions

This section describes how to rebuild the artifact and reproduce the results in
more detail. The full experiments are optional because they are expensive in
runtime, memory, and storage.

### Load Docker Image and Build Binaries

Estimated cost: about 10 minutes, moderate-to-high CPU load, recommended
`>=128GB` RAM, and about `21GB` of additional disk for build artifacts and the
loaded Docker image.

Load the prebuilt Docker image included with the artifact package, then build
libCacheSim and CacheLib from the mounted source tree:

```bash
docker pull ghcr.io/telos-syslab/merlin-ae:ae-v1
docker tag ghcr.io/telos-syslab/merlin-ae:ae-v1 merlin-ae:latest
docker run --rm --network=host --cap-add=SYS_NICE -v "$(pwd)":/Merlin -w /Merlin merlin-ae /bin/bash -lc 'bash scripts/install.sh'
```

`scripts/install_dependency.sh` installs the system packages and builds zstd. `scripts/install.sh` builds two libCacheSim variants and CacheLib:

- `libCacheSim/_build/`: normal build for the main simulations.
- `libCacheSim/_build2/`: build with `TRACK_PARAMETERS` enabled for precision experiments.
- `CacheLib/mybench/_build/`: normal build for eviction algorithms in CacheLib.

Artifact maintainers can regenerate the image tarball from the Dockerfile with:

```bash
bash scripts/package_docker_image.sh
```

This produces `merlin-ae-image.tar.gz` and `merlin-ae-image.tar.gz.sha256`.

### Downloaded Datasets Layout

Estimated resource cost: about `2TB` download size; download time depends on
network bandwidth and can range from a few hours to a few days.

The Figure 11-13 simulations use public datasets from the cache-dataset archive:

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

The full datasets are large. For spot checks, download the datasets needed for
the experiment you want to inspect.

### Figure 11-13: Hit Rate, Byte Hit Rate, and Relative Hit Ratio

Recommended AE path: use the bundled results and plotting scripts.

Estimated cost for a full rerun: about `1M CPU-hours` in aggregate, `>=1TB`
RAM for aggressive parallel execution, and about `2TB` of dataset storage plus
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
docker run --rm --network=host --cap-add=SYS_NICE -v "$(pwd)":/Merlin -w /Merlin merlin-ae /bin/bash -lc '
python3 scripts/evaluation.py \
  --root_dir ./libCacheSim/_build \
  --input_dir ./CacheTrace \
  --output_dir ./results/eval_ignore_obj \
  --ignore_obj
'

# Object size considered: used for the byte-hit-rate figure.
docker run --rm --network=host --cap-add=SYS_NICE -v "$(pwd)":/Merlin -w /Merlin merlin-ae /bin/bash -lc '
python3 scripts/evaluation.py \
  --root_dir ./libCacheSim/_build \
  --input_dir ./CacheTrace \
  --output_dir ./results/eval_with_obj
'
```

If you want to run the trace step by step instead of the script, please run:
```bash
# For object hit rate
docker run --rm --network=host --cap-add=SYS_NICE -v "$(pwd)":/Merlin -w /Merlin merlin-ae /bin/bash -lc '
./libCacheSim/_build/bin/cachesim <path/to/data> <format,oracleGeneral> <algorithm> <cache size ratio> --num-thread N --outputdir <output_dir> --ignore-obj-size=true
'

# For byte hit rate
docker run --rm --network=host --cap-add=SYS_NICE -v "$(pwd)":/Merlin -w /Merlin merlin-ae /bin/bash -lc '
./libCacheSim/_build/bin/cachesim <path/to/data> <format,oracleGeneral> <algorithm> <cache size ratio> --num-thread N --outputdir <output_dir>
'
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

Estimated cost for the optional rerun: high CPU load, recommended `>=32GB` RAM,
and about `6GB` of disk for the synthetic trace.

**Attention: We recommend running the throughput experiment on a machine with at least `32 cores.`**

The precomputed throughput results are in:

- `data/throughput/wback/`
- `data/throughput/woback/`

Regenerate the figure:

```bash
python3 scripts/plot/throughput.py
```

To reproduce the throughput data, first generate the synthetic mixed trace:

```bash
python3 ./scripts/data_genmix.py \
  -m 1000000 \
  -n 200000000 \
  --bin-output ./CacheTrace/mix.oracleGeneral.bin
```

Then run the CacheLib microbenchmark inside the `merlin-ae` Docker image.
The helper script `scripts/throughput.sh` records the algorithm, cache-size,
hash-power, and thread-count settings used for this experiment.

```bash
bash scripts/throughput.sh woback
bash scripts/throughput.sh wback
```

*These experiments might consume 10h or more.*

### Figure 15: Flash-Cache Hit Rate and Write Bytes

Recommended AE path: use the bundled results and `scripts/plot/flash.py`.

Estimated cost for the optional rerun: high CPU load, recommended `>=32GB` RAM,
and about `8.5GB` of disk for the CloudPhysics dataset.

The precomputed flash-cache results are in `data/flash/`. Regenerate the figure:

```bash
python3 scripts/plot/flash.py
```

To reproduce the raw flash-cache measurements, download the CloudPhysics dataset:

<https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/cacheDatasets/cloudphysics/>

Place the dataset under:

```text
CacheTrace/cloudphysics/
```

Then run:

```bash
docker run --rm --network=host --cap-add=SYS_NICE -v "$(pwd)":/Merlin -w /Merlin merlin-ae /bin/bash -lc '
MAX_JOBS=2 bash scripts/flash.sh
'
```

`MAX_JOBS` controls the number of concurrent flash-cache jobs. Increase it only
if the machine has enough CPU and memory. The script writes raw outputs under
`data/flash/<cache-size>/`. Use `scripts/process_flash.py` to summarize raw
outputs into the `data/flash/*.txt` files consumed by `scripts/plot/flash.py`.

If you want to get the results of Flashield, please use the following instruction:
```bash
python3 -m pip install scikit-learn lru-dict
# The data should be decompress before running.
python3 scripts/flashield/flashield.py /path/to/data --ram-size-ratio=0.001 --disk-cache-type=FIFO --use-obj-size true
python3 scripts/flashield/flashield.py /path/to/data --ram-size-ratio=0.01 --disk-cache-type=FIFO --use-obj-size true
python3 scripts/flashield/flashield.py /path/to/data --ram-size-ratio=0.10 --disk-cache-type=FIFO --use-obj-size true
```
You can take the last line as the result.

Because it needs machine learning to obtain the results, so we recommend you to use a machine with abundant GPU resources.

*The experimental instructions are similar to those used in Figure 11-13. Please refer to the script for specific details.*

### Figure 16: Merlin Sensitivity Evaluation

Recommended AE path: use the bundled processed inputs in `data/sensitivity/`.

Estimated cost for the optional rerun: very high CPU load, recommended
`>=128GB` RAM, and about `2TB` of datasets storage plus outputs.

Figure 16 studies Merlin's sensitivity to the `filter-size-ratio`,
`staging-size-ratio`, and `ghost-size-ratio` parameters. The precomputed
`.dat` files already in `data/sensitivity/` contain the percentile summaries
used by the paper.

**To get the baseline `LRU` results, you can run scripts in Figure 11-13 or with:**
```bash
docker run --rm --network=host --cap-add=SYS_NICE -v "$(pwd)":/Merlin -w /Merlin merlin-ae /bin/bash -lc '
python3 scripts/evaluation.py \
  --root_dir ./libCacheSim/_build \
  --input_dir ./CacheTrace \
  --output_dir ./results/eval_ignore_obj \
  --ignore_obj --policy_list lru
'

docker run --rm --network=host --cap-add=SYS_NICE -v "$(pwd)":/Merlin -w /Merlin merlin-ae /bin/bash -lc '
python3 scripts/readeval.py \
  --input_dir ./results/eval_ignore_obj \
  --output_dir ./dataresult/withoutobjsize \
  --normalize_policy
'
```

To reproduce the raw sensitivity evaluation, run Merlin-only simulations on the
same datasets used for Figures 11-13:

```bash
docker run --rm --network=host --cap-add=SYS_NICE -v "$(pwd)":/Merlin -w /Merlin merlin-ae /bin/bash -lc '
python3 scripts/sensitivity.py \
  --root_dir ./libCacheSim/_build \
  --input_dir ./CacheTrace \
  --output_dir ./results/sensitivity \
  --ignore_obj
'
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

`scripts/getSen.py` aggregates the selected datasets, computes the relative hit
ratio against `LRU`, and emits one `.dat` file per cache-size and ghost-size
slice.

*The experimental instructions are similar to those used in Figure 11-13. Please refer to the script for specific details.*

### Figure 17: Access-Pattern Precision

Recommended AE path: use the bundled `data/precision/*.dat` files and
`scripts/plot/precision.py`.

Estimated cost for the optional rerun: high CPU load, recommended `>=128GB` RAM,
and the same datasets storage needed for the precision dataset.

The parameter-tracking build is created under `libCacheSim/_build2/`. Prepare
the Twitter and FIU traces listed below, then run:

```bash
docker run --rm --network=host --cap-add=SYS_NICE -v "$(pwd)":/Merlin -w /Merlin merlin-ae /bin/bash -lc '
bash scripts/precision.sh
'
python3 scripts/plot/precision.py data/precision/fiu.dat -o fiu.pdf
python3 scripts/plot/precision.py data/precision/twitter.dat -o twitter.pdf
```

*The experimental instructions are similar to those used in Figure 11-13. Please refer to the script for specific details.*

## Notes for Reviewers

- Use the Getting Started path for the lowest-cost AE check.
- Full reproduction is available but expensive in CPU time, memory, and
  storage.
- The scripts are resumable and skip completed results when possible.
- No private datasets are required; all large datasets are publicly available.
- The installation scripts use `sudo apt`, `sudo make install`, and Docker.
