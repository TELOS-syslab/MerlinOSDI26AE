# Artifact Evaluation Guide

## Setup
An x86-64 machine with an arbitrary Linux install is suffice. We've tested Merlin on an AMD machine with 384 cores (two Epyc 9965 processors).

```
git clone --recursive https://github.com/TELOS-syslab/MerlinOSDI26AE.git
cd MerlinOSDI26AE
```

The following guide will assume that you are in this directory.

## Install Dependency
```
bash ./scripts/install_dependency.sh
```

## Install libCacheSim and CacheLib
```
bash ./scripts/install.sh
```

Congratulations! Now you have installed libCacheSim and CacheLib. 

## Reproducing Performance Evaluation Figures
This may take several hours.

```
bash ./scripts/run_all_experiments.sh
```

Then we could have the raw data and run the plot script.

```
bash ./scripts/run_all_plot.sh
```

Or, you can run the following instructions to reproduce our results step by step.

### Figure 14, throughput of the evaluated algorithms.

### Figure 15, write bytes (normalized to trace size) and hit rate in Cloudphysics.
The traces used in this experiment can be downloaded at [Cloudphysics](https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/cacheDatasets/cloudphysics/). We assume that all traces are downloaded and placed in the folder `./raw_data`.

```
# decompress traces for flashield

# 
```