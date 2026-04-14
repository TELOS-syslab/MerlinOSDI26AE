#include "log.h"

static constexpr int MAX_RECORDS = 1000000;
std::vector<uint64_t> access_records{std::vector<uint64_t>(MAX_RECORDS,0)};
std::atomic<size_t> record_index{0};



void log(uint64_t val)
{
    if (record_index > MAX_RECORDS)
    {
        return;
    }
    size_t index = record_index.fetch_add(1, std::memory_order_relaxed);
    if (index < MAX_RECORDS)
    {
        access_records[index] = val;
    }
    return;
}

void dump(std::ostream &out)
{
    int total_size = std::min(MAX_RECORDS, (int)record_index.load(std::memory_order_relaxed));
    for (size_t i = 0; i < total_size; ++i)
    {
        out << access_records[i] << "\n";
    }
    return;
}

void analysis(std::istream &in){
    uint64_t total = 0;
    uint64_t counter = 0;
    uint64_t time = 0;
    while(in>>time){
        total += time;
        counter++;
    }
    printf("counter %ld, average %lf\n",counter, (double)total/(double)counter);
    return;
}