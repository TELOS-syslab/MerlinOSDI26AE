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


### Figure 11, Hit rate of evaluated algorithms.
The provided results are in [/data/HR/withobjsize](./data/HR/withobjsize/).
#### Plot the figures using the results
```
python scripts/plot/hit_rate.py 
```
This generates `hit_rate.pdf` which is Figure 11.


### Figure 12, Byte hit rate of evaluated algorithms. 
The provided results are in [/data/HR/withobjsize](./data/HR/withobjsize/).
```
python scripts/plot/byte_hit_rate.py 
```

This generates `byte_hit_rate.pdf` which is Figure 12.


### Figure 13, Relative hit rate compared to the dominant algorithm.
The provided results are in [/data/RHR/withoutobjsize](./data/RHR/withoutobjsize/).

#### Plot the figures using the results
```
python scripts/plot/relative_hit_ratio.py 
```
This generates `relative_hit_ratio.pdf` which is Figure 13.


### Figure 14, throughput of the evaluated algorithms.
#### Plot the figures using the results
```

```

### Figure 15, write bytes (normalized to trace size) and hit rate in Cloudphysics.
The traces used in this experiment can be downloaded at [Cloudphysics](https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/cacheDatasets/cloudphysics/). 

```

```

### Figure 17, 