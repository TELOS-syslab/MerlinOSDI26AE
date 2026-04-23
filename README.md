# Merlin OSDI26 Artifact Evaluation Guide
The artifact of the OSDI'26 paper "Merlin: An Efficient Adaptive Cache Eviction
Algorithm via Fine-Grained Characterization".

Thanks for your willingness to evaluate our code and reproduce the claims in the paper.

## Setup
An x86-64 machine with an Ubuntu 22.04 LTS is suffice. We've tested Merlin on an AMD machine with 384 cores (two Epyc 9965 processors).

```
git clone --recursive https://github.com/TELOS-syslab/MerlinOSDI26AE.git
cd MerlinOSDI26AE
```

The following guide will assume that you are in the MerlinOSDI26AE directory.

## Install Dependency
We use libCacheSim and CacheLib to perform evaluations, which can be installed using the following commands.

This may take XXX minutes.
```
bash ./scripts/install_dependency.sh
bash ./scripts/install.sh
```

Congratulations! Now you have installed libCacheSim and CacheLib. 

## Reproduce the results and figures
> **Note** 
> It requires a lot of time and disk space to finish the whole evaluation, so we will provide you with the results to plot figures.
> The full dataset is large (about 2 TB), so we recommend you to download traces you need only.
> It will take a long time (about 1 million CPU hours) to finish the whole evaluation. Although libCacheSim could run experiments in parallel, it needs large memory. 
> If you would like to reduce DRAM usase, you can run single experiment each time.


```bash
# Please install pip for evaluation and plotting figures.
pip install pandas, numpy, matplotlib, psutil
```


### Figure 11~13, 
> **Note** We hope you download datasets in directory CacheTrace, or you could replace the input dir to `path/to/all/dataset` in the following commands.
> The traces used in this experiment can be downloaded at [cacheDatasets](https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/cacheDatasets/)
>
> We construct the datasets directory as, please ignore the sampled datasets and traces too small in datasets.
>  CacheTrace
> - [alibabaBlock/](https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/cacheDatasets/alibabaBlock/old/)
> - [cloudphysics/](https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/cacheDatasets/cloudphysics/)
> - [fiu/](https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/cacheDatasets/fiu/)
> -	[metaCDN/](https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/cacheDatasets/metaCDN/)
> - [metaKV/](https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/cacheDatasets/metaKV/)
> - [msr/](https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/cacheDatasets/msr/)
> - [systor/](https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/cacheDatasets/systor/)
> - [tencentBlock/](https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/cacheDatasets/tencentBlock/old/)
> - [tencentPhoto/](https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/cacheDatasets/tencentPhoto/)
> - [twitter/](https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/cacheDatasets/twitter/)
> -	[wiki/](https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/cacheDatasets/wiki/)

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

#### [Optional] Verify the simulation results
Running evaluation (Estimate evaluation time: 1 million CPU hours)
```bash
#running evaluation commands python scripts/evaluation.py --root_dir <root/dir/of/libcCacheSim> --input_dir <path/to/all/dataset> --output_dir <outputdir> 

#--ignore_obj controls whether to consider the object size
python scripts/evaluation.py --root_dir ./libCacheSim/_build/ --input_dir ./CacheTrace --output_dir ./results/evaligobj --ignore_obj

python scripts/evaluation.py --root_dir ./libCacheSim/_build/ --input_dir ./CacheTrace --output_dir ./results/evalobj
```

Processing results
```bash
#Collect evaluation results in different cache size
python scripts/readeval.py --input_dir ./results/evaligobj --output_dir ./dataresult/withoutobjsize/ --normalize_policy

python scripts/readeval.py --input_dir ./results/evalobj --output_dir ./dataresult/withobjsize/ --normalize_policy

#Calculate the relative hit improvement 
python scripts/getHR.py --input_dir ./dataresult/withoutobjsize/ --output_dir ./data/HR/withoutobjsize

python scripts/getHR.py --input_dir ./dataresult/withobjsize/ --output_dir ./data/HR/withobjsize
```

### Figure 14, throughput of the evaluated algorithms.
#### Plot the figures using the results
```bash
python3 scripts/plot/throughput.py
```

This generates `throughput.pdf` which is Figure 14.

#### [Optional] Reproduce the results
(Estimate evaluation time: )
```bash
# Generate the trace.
python ./CacheLib/mybench/data_genmix.py -m 1000000 -n 100000000 --bin-output ./mix.oracleGeneral.bin

# Run the experiment with backend
docker run --rm --cap-add=SYS_NICE -it -v "$(pwd)":/Merlin -w /Merlin cachelib-ae /bin/bash -lc "bash scripts/with_backend.sh && bash scripts/without_backend.sh"

# Process the results
```

### Figure 15, write bytes (normalized to trace size) and hit rate in Cloudphysics.
The traces used in this experiment can be downloaded at [Cloudphysics](https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/cacheDatasets/cloudphysics/). 

```

```

### Figure 16
```bash
./_build2/bin/cachesim ./CacheTrace/twitter/cluster8.oracleGeneral.zst oracleGeneral merlin 0.1 --ignore-obj-size=true
./_build2/bin/cachesim ./CacheTrace/twitter/cluster8.oracleGeneral.zst oracleGeneral cacheus 0.1 --ignore-obj-size=true
./_build2/bin/cachesim ./CacheTrace/twitter/cluster8.oracleGeneral.zst oracleGeneral arc 0.1 --ignore-obj-size=true

./_build2/bin/cachesim ./CacheTrace/fiu/fiu_webmail.cs.fiu.edu-110108-113008.oracleGeneral.zst oracleGeneral merlin 0.2 --ignore-obj-size=true
./_build2/bin/cachesim ./CacheTrace/fiu/fiu_webmail.cs.fiu.edu-110108-113008.oracleGeneral.zst oracleGeneral cacheus 0.2 --ignore-obj-size=true
./_build2/bin/cachesim ./CacheTrace/fiu/fiu_webmail.cs.fiu.edu-110108-113008.oracleGeneral.zst oracleGeneral arc 0.2 --ignore-obj-size=true
```