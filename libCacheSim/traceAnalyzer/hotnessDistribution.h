#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <unordered_map>
#include <vector>

#include "../include/libCacheSim/reader.h"
#include "struct.h"
#include "utils/include/utils.h"

namespace traceAnalyzer {
class HotnessDistribution {
    public:
    HotnessDistribution(std::string output_path, int epoch_size, int max_freq):
    counter_(0),
    epoch_size_(epoch_size), max_freq_(max_freq),
    hotness_distribution_(max_freq+1, 0) { 
        turn_on_stream_dump(output_path);
    };

    ~HotnessDistribution(){
        stream_dump_hd_ofs.close();
    };

    void add_req(request_t *req){
        obj_id_t obj_id = req->obj_id;
        auto it = obj_access_.find(obj_id);
        counter_ ++;
        if(it == obj_access_.end()){
            obj_access_[obj_id] = 1;
            hotness_distribution_[1] += 1;
        } else {
            int32_t prev_freq = it->second;
            if(prev_freq >= max_freq_){
                return;
            }
            hotness_distribution_[prev_freq] -= 1;
            obj_access_[obj_id] += 1;
            hotness_distribution_[obj_access_[obj_id]] += 1;
        }
        if(counter_ >= epoch_size_){
            stream_dump_hotness_distribution();
            counter_ = 0;
            obj_access_.clear();
            std::fill(hotness_distribution_.begin(), hotness_distribution_.end(), 0);
        }
        return;
    }

    void dump(std::string &path_base){
        if(counter_ > epoch_size_ / 2){
            stream_dump_hotness_distribution();
        }
        stream_dump_hd_ofs.close();
    }

    void turn_on_stream_dump(std::string &path_base){
        stream_dump_hd_ofs.open(path_base + ".hotness_distribution", std::ios::out | std::ios::trunc);
    }

    void stream_dump_hotness_distribution(){
        for(int i = 1; i <= max_freq_; i++){
            stream_dump_hd_ofs << hotness_distribution_[i];
            if(i != max_freq_){
                stream_dump_hd_ofs << ",";
            }
        }
        stream_dump_hd_ofs << std::endl;
    }

    int counter_;
    int epoch_size_;
    int max_freq_;
    std::unordered_map<obj_id_t, int32_t> obj_access_;
    std::vector<uint32_t> hotness_distribution_;
    std::ofstream stream_dump_hd_ofs;
};// end class hotnessDistribution
}// end namespace traceAnalyzer