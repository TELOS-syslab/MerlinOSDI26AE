#include <atomic>
#include <vector>
#include <x86intrin.h>
#include <iostream>

extern std::vector<uint64_t> access_records;
extern std::atomic<size_t> record_index;

inline unsigned long long rdtscp_now()
{
    unsigned int aux;
    return __rdtscp(&aux);
}
void log(uint64_t val);
void dump(std::ostream &out);
void analysis(std::istream &in);