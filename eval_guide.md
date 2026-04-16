# Artifact Evaluation Guide

## Setup
An x86-64 machine with an arbitrary Linux install is suffice. We've tested Merlin on an AMD machine with 384 cores (two Epyc 9965 processors).

```
git clone https://github.com/TELOS-syslab/MerlinOSDI26AE.git
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

## Reproducing Performance Evaluation Figures
This may take several hours.

```
bash ./scripts/run_all_experiments.sh
```

Then we could have the raw data and run the plot script.

```
bash ./scripts/run_all_plot.sh
```