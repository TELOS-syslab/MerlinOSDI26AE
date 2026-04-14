/**
 * @file accessPattern.cpp
 * @author Juncheneg Yang (peter.waynechina@gmail.com)
 * @brief calculate the access pattern of the trace, and dump the result to file
 *
 * @version 0.1
 * @date 2023-06-16
 *
 * @copyright Copyright (c) 2023
 *
 */

#include "accessPattern.h"
#include "../include/libCacheSim/request.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std;

namespace traceAnalyzer {

void AccessPattern::add_req(const request_t *req) {
  if (n_seen_req_ > 0xfffffff0) {
    if (n_seen_req_ == 0xfffffff0) {
      INFO("trace is too long, accessPattern uses up to 0xfffffff0 requests\n");
    }
    return;
  }
  n_seen_req_ += 1;
  if(cache != nullptr){
    if(n_seen_req_ < 10){
        print_const_request(req, 0,0);
    }
      if(cache->get(cache, req)==true){
        cache_contents.insert(req);
        return;
      }
  }

  if (start_rtime_ == -1) {
    start_rtime_ = req->clock_time;
  }
  
  //if (req->obj_id % sample_ratio_ != 0) {
    /* skip this object */
  //  return;
  //}

  for (set<const request_t *>::iterator it=cache_contents.begin(); it!=cache_contents.end(); ++it){
    const request_t * find_req = *it;
    if (cache->find(cache, find_req, false) == NULL){
      if (evict_rtime_map_.find(find_req->obj_id) == evict_rtime_map_.end()) {
        evict_rtime_map_[find_req->obj_id] = vector<uint32_t>();
        evict_vtime_map_[find_req->obj_id] = vector<uint32_t>();
      }
      evict_rtime_map_[find_req->obj_id].push_back(req->clock_time - start_rtime_);
      evict_vtime_map_[find_req->obj_id].push_back(n_seen_req_);
      cache_contents.erase(find_req);
      break;
    }
  }
  cache_contents.insert(req);

  if (access_rtime_map_.find(req->obj_id) == access_rtime_map_.end()) {
    access_rtime_map_[req->obj_id] = vector<uint32_t>();
    access_vtime_map_[req->obj_id] = vector<uint32_t>();
  }
  access_rtime_map_[req->obj_id].push_back(req->clock_time - start_rtime_);
  access_vtime_map_[req->obj_id].push_back(n_seen_req_);
}

void AccessPattern::dump(string &path_base) {

  string ofile_path = path_base + ".accessRtime";
  ofstream ofs(ofile_path, ios::out | ios::trunc);
  //ofs << "# " << path_base << "\n";
  //ofs << "# access pattern real time, each line stores all the real time of "
  //     "requests to an object\n";

  // sort the timestamp list by the first timestamp
  vector<vector<uint32_t> *> sorted_rtime_vec;
  for (auto &p : access_rtime_map_) {
    sorted_rtime_vec.push_back(&p.second);
  }
  sort(sorted_rtime_vec.begin(), sorted_rtime_vec.end(),
       [](vector<uint32_t> *p1, vector<uint32_t> *p2) {
         return (*p1).at(0) < (*p2).at(0);
       });

  for (const auto *p : sorted_rtime_vec) {
    for (const uint32_t rtime : *p) {
      ofs << rtime << ",";
    }
    ofs << "\n";
  }
  ofs << "\n" << endl;
  ofs.close();

  //ofs << "# " << path_base << "\n";

  string ofile_path2 = path_base + ".accessVtime";
  ofstream ofs2(ofile_path2, ios::out | ios::trunc);
  //ofs2 << "# access pattern virtual time, each line stores all the virtual "
  //        "time of requests to an object\n";
  vector<vector<uint32_t> *> sorted_vtime_vec;
  for (auto &p : access_vtime_map_) {
    sorted_vtime_vec.push_back(&(p.second));
  }

  sort(sorted_vtime_vec.begin(), sorted_vtime_vec.end(),
       [](vector<uint32_t> *p1, vector<uint32_t> *p2) -> bool {
         return (*p1).at(0) < (*p2).at(0);
       });

  for (const vector<uint32_t> *p : sorted_vtime_vec) {
    for (const uint32_t vtime : *p) {
      ofs2 << vtime << ",";
    }
    ofs2 << "\n";
  }

  ofs2.close();


  string ofile_path3 = path_base + ".evictRtime";
  ofstream ofs3(ofile_path3, ios::out | ios::trunc);
  vector<vector<uint32_t> *> sorted_evictrtime_vec;
  for (auto &p : evict_rtime_map_) {
    sorted_evictrtime_vec.push_back(&p.second);
  }
  sort(sorted_evictrtime_vec.begin(), sorted_evictrtime_vec.end(),
       [](vector<uint32_t> *p1, vector<uint32_t> *p2) {
         return (*p1).at(0) < (*p2).at(0);
       });
  for (const auto *p : sorted_evictrtime_vec) {
    for (const uint32_t rtime : *p) {
      ofs3 << rtime << ",";
    }
    ofs3 << "\n";
  }
  ofs3 << "\n" << endl;
  ofs3.close();


  string ofile_path4 = path_base + ".evictVtime";
  ofstream ofs4(ofile_path4, ios::out | ios::trunc);
  vector<vector<uint32_t> *> sorted_evictvtime_vec;
  for (auto &p : evict_vtime_map_) {
    sorted_evictvtime_vec.push_back(&(p.second));
  }
  sort(sorted_evictvtime_vec.begin(), sorted_evictvtime_vec.end(),
       [](vector<uint32_t> *p1, vector<uint32_t> *p2) -> bool {
         return (*p1).at(0) < (*p2).at(0);
       });
  for (const vector<uint32_t> *p : sorted_evictvtime_vec) {
    for (const uint32_t vtime : *p) {
      ofs4 << vtime << ",";
    }
    ofs4 << "\n";
  }
  ofs4.close();
}

};  // namespace traceAnalyzer
