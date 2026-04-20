#!/usr/bin/env python3
"""
example usage
for i in 0.2 0.4 0.6 0.8 1 1.2 1.4 1.6; do 
    python3 data_gen.py -m 1000000 -n 100000000 --alpha $i > /disk/data/zipf_${i}_1_100.txt & 
done

for i in 0.2 0.4 0.6 0.8 1 1.2 1.4 1.6; do 
    python3 data_gen.py -m 10000000 -n 100000000 --alpha $i --bin-output /disk/data/zipf_${i}_10_100.oracleGeneral & 
done


"""

from functools import *
import random
import bisect
import math
import numpy as np
import struct

from collections import deque

class ZipfGenerator:

    def __init__(self, m, alpha):
        # Calculate Zeta values from 1 to n:
        tmp = [1. / (math.pow(float(i), alpha)) for i in range(1, m + 1)]
        zeta = reduce(lambda sums, x: sums + [sums[-1] + x], tmp, [0])

        # Store the translation map:
        self.distMap = [x / zeta[-1] for x in zeta]

    def next(self):
        # Take a uniform 0-1 pseudo-random value:
        u = random.random()

        # Translate the Zipf variable:
        return bisect.bisect(self.distMap, u) - 1

class LRU:
    def __init__(self, capacity,start_id=0):
        self.capacity = capacity
        self.queue = deque()  # 存放 item IDs
        self.start_id = start_id
        self.gen_id = 0
        
    def insert(self, item):
        if item in self.queue:
            self.queue.remove(item)
        elif len(self.queue) >= self.capacity:
            self.queue.pop()  # 淘汰最久未用的
        self.queue.appendleft(item)

    def access_by_reuse_distance(self, reuse_distance):
        if reuse_distance < len(self.queue):
            # 命中：取出对应位置的元素（0是最近的）
            idx = reuse_distance
            item = self.queue[idx]
            del self.queue[idx]
            self.queue.appendleft(item)
            return item  # hit
        else:
            # miss: 插入新元素（用一个新 ID）
            #new_item = f"item_{random.randint(0, 10**9)}"
            new_item = self.gen_id
            self.gen_id += 1
            if len(self.queue) >= self.capacity:
                self.queue.pop()  # 淘汰最久未用的
            self.queue.appendleft(new_item)
            return new_item + self.start_id # miss

def gen_zipf(m: int, alpha: float, n: int, start: int = 0) -> np.ndarray:
    """generate zipf distributed workload

    Args:
        m (int): the number of objects
        alpha (float): the skewness
        n (int): the number of requests
        start (int, optional): start obj_id. Defaults to 0.

    Returns:
        requests that are zipf distributed 
    """

    np_tmp = np.power(np.arange(1, m + 1), -alpha)
    np_zeta = np.cumsum(np_tmp)
    dist_map = np_zeta / np_zeta[-1]
    r = np.random.uniform(0, 1, n)
    return np.searchsorted(dist_map, r) + start


def gen_uniform(m: int, n: int, start: int = 0) -> np.ndarray:
    """generate uniform distributed workload

    Args:
        m (int): the number of objects
        n (int): the number of requests
        start (int, optional): start obj_id. Defaults to 0.

    Returns:
        requests that are uniform distributed
    """

    return np.random.uniform(0, m, n).astype(int) + start

def gen_data(m: int, repeat: int, start: int = 0)-> np.ndarray:
    """generate workload

    Args:
        m (int): the number of objects
        n (int): the number of requests
        start (int, optional): start obj_id. Defaults to 0.

    Returns:
        requests that are distributed
    """
    first_half = np.arange(start, start + m)
    #repeat the first half
    repeated = np.tile(first_half, repeat)
    return repeated

#python3 ./data_genmix.py -m 1000000 -n 100000000 --bin-output mix2.oracleGeneral.bin
if __name__ == "__main__":
    from argparse import ArgumentParser
    ap = ArgumentParser()
    ap.add_argument("-m", type=int, default=1000000, help="Number of objects")
    ap.add_argument("-n",
                    type=int,
                    default=200000000,
                    help="Number of requests")
    ap.add_argument("--alpha", type=float, default=1.0, help="Zipf parameter")
    ap.add_argument("--bin-output",
                    type=str,
                    default="",
                    help="Output to a file (oracleGeneral format)")
    ap.add_argument("--obj-size",
                    type=int,
                    default=4000,
                    help="Object size (used when output to a file)")
    ap.add_argument("--time-span",
                    type=int,
                    default=86400 * 7,
                    help="Time span of all requests in seconds")

    p = ap.parse_args()

    output_file = open(p.bin_output, "wb") if p.bin_output != "" else None
    s = struct.Struct("<IQIq")

    batch_size = 500000
    lru = LRU(1000000, start_id=p.m)
    '''
    ziplruf = []
        for rd in zipf:
            ziplruf.append(rd)
            lru.insert(rd)
            ziplruf.append(lru.access_by_reuse_distance(rd))
    '''
    i = 0
    #fill the cache
    
    zipf = gen_zipf(p.m, p.alpha, batch_size)
    small_size = int(p.m * 0.1)
    small_base = p.m
    smallchurn = gen_data(small_size, 1, small_base)
    mid_size = int(p.m * 0.4)
    mid_base = p.m + small_size
    midscan = gen_data(mid_size, 1, mid_base)
    rand_size = int(p.m * 0.32)
    rand_base = p.m + small_size + mid_size
    unirand = gen_data(rand_size, 20, rand_base)
    uniform_size = int(p.m * 0.24)
    uniform_base = p.m + small_size + mid_size + rand_size
    unif = gen_uniform(uniform_size, uniform_size*15, uniform_base)
    frequency_size = int(p.m * 0.2)
    frequency_base = p.m + small_size + mid_size + rand_size + uniform_size
    freqf = gen_data(frequency_size, 45, frequency_base)
    scan_size = int(p.m * 0.6)
    scan_base = p.m + small_size + mid_size + rand_size + uniform_size + frequency_size
    bigscan = gen_data(scan_size, 1, scan_base)
    shuffled = np.concatenate((unirand, unif,zipf,freqf))
    random.shuffle(shuffled)
    warmup = np.concatenate((smallchurn, midscan, smallchurn, shuffled, bigscan))
    shuffled_split1 = shuffled[0:len(shuffled)//2]
    shuffled_split2 = shuffled[len(shuffled)//2:]
    second = np.concatenate((smallchurn, shuffled_split1,smallchurn,midscan, shuffled_split2,midscan,bigscan))
    print(len(warmup), len(second))
    for obj in warmup:
        i += 1
        ts = i * p.time_span // p.n
        if output_file:
            output_file.write(s.pack(ts, obj, p.obj_size, -2))
        else:
            print(obj)
    
    for n_batch in range((p.n - 1) // len(second) + 1):
        for obj in second:
            i += 1
            ts = i * p.time_span // p.n
            if output_file:
                output_file.write(s.pack(ts, obj, p.obj_size, -2))
            else:
                print(obj)
        scan_base += scan_size
        bigscan = gen_data(scan_size, 1, scan_base)
        second = np.concatenate((smallchurn, shuffled_split1,smallchurn,midscan, shuffled_split2,midscan,bigscan))
    exit(0)
