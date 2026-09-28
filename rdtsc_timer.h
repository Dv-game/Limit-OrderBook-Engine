// rdtsc_timer.h
#pragma once

#ifdef _MSC_VER
    #include <intrin.h>     // For MSVC compiler
#else
    #include <x86intrin.h>  // For GCC/Clang compilers (CLion default)
#endif

namespace Profiling {
    // Reads the CPU's internal clock cycle counter
    inline unsigned long long get_cpu_cycles() {
        // _mm_lfence() acts as a memory barrier so the CPU doesn't reorder
        // instructions and cheat the timer
        _mm_lfence();
        unsigned long long cycles = __rdtsc();
        _mm_lfence();
        return cycles;
    }
}