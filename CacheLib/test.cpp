#include <x86intrin.h>
#include <cstdio>
#include <chrono>
#include <thread>

inline unsigned long long rdtscp_now()
{
    unsigned int aux;
    return __rdtscp(&aux);
}

int main(){
    auto start = rdtscp_now();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto end = rdtscp_now();
    printf("rdtsc %ld\n", end - start);
    return 0;
}