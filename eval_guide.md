# Artifact Evaluation Guide

## Setup
An x86-64 machine with an arbitrary Linux install is suffice. We've tested Merlin on an AMD machine with 384 cores (two Epyc 9965 processors).

```
git clone --recursive https://github.com/TELOS-syslab/MerlinOSDI26AE.git
cd MerlinOSDI26AE
```

The following guide will assume that you are in this directory.

## Install Dependency
We use libCacheSim and CacheLib to perform simulations, which can be installed using the following commands.
```
bash ./scripts/install_dependency.sh
bash ./scripts/install.sh
```

Congratulations! Now you have installed libCacheSim and CacheLib. 

## Reproduce the results and figures
We required you to install some python librarys to run the experiments and plot the figures.
```bash
# Please install pip first.
pip install pandas, numpy, matplotlib, psutil
```


### Figure 11~13, 
> **Warning** These experiment for Figure 11 to 13 takes long time to finish, we don't expect reviewers to finish them within the deadline. So we provide already computed results so that reviewers can spot check and plot them.

The provided results are in [/data/HR](./data/HR) and [/data/RHR](./data/RHR/).
#### Plot the figures using the results
```bash
# Hit rate of evaluated algorithms.
python scripts/plot/hit_rate.py 

# Byte hit rate of evaluated algorithms. 
python scripts/plot/byte_hit_rate.py 

# Relative hit rate compared to the dominant algorithm.
python scripts/plot/relative_hit_ratio.py 
```
This generates `hit_rate.pdf` which is Figure 11, `byte_hit_rate.pdf` which is Figure 12, and `relative_hit_ratio.pdf` which is Figure 13.

#### Run the simulator
```
python scripts/evaluation.py --root_dir ./libCacheSim/_build/ --input_dir /path/to/all_trace --output_dir results --ignore_obj
```
**Note:** The `path/to/all_trace` 


### Figure 14, throughput of the evaluated algorithms.
#### Reproduce the results
```bash
# Generate the trace.
python CacheLib/mybench/data_genmix.py --bin-output ./raw_data/mix.oracleGeneral.bin 

# Run the experiment with backend
docker run --rm --cap-add=SYS_NICE -it -v "$(pwd)":/Merlin -w /Merlin cachelib-ae /bin/bash -lc "bash scripts/with_backend.sh && bash scripts/without_backend.sh"

# Process the results

```
#### Plot the figures using the results
```bash
python3 scripts/plot/throughput.py
```
This generates `throughput.pdf` which is Figure 14.

### Figure 15, write bytes (normalized to trace size) and hit rate in Cloudphysics.
The traces used in this experiment can be downloaded at [Cloudphysics](https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/cacheDatasets/cloudphysics/). 

```

```

### Figure 17
