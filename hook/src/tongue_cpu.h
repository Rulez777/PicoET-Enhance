#pragma once
// Places tongue threads on the fastest CPU cores, off the cores the eye/face tracking service is
// confined to. The cores are found from cpufreq, not assumed: every core whose maximum frequency
// is at least 75% of the fastest one (on the XR2 in PICO 4 Pro/Enterprise: the gold and prime
// cores). Without cpufreq the upper half of the cores is used.
#include <cstdio>
#include <fcntl.h>
#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>

namespace tongue_cpu {
static int fast_cores(cpu_set_t& set, int skip_first = 0) {
    long freq[64] = {}; long best = 0; int ncpu = 0;
    for (int c = 0; c < 64; c++) {
        char path[96];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d", c);
        if (access(path, F_OK) != 0) break;   // no such core: done
        ncpu = c + 1;
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
        if (FILE* f = fopen(path, "re")) {
            if (fscanf(f, "%ld", &freq[c]) != 1) freq[c] = 0;
            fclose(f);
        }
        if (freq[c] > best) best = freq[c];
    }
    if (ncpu == 0) ncpu = int(sysconf(_SC_NPROCESSORS_CONF));
    CPU_ZERO(&set); int chosen = 0, skipped = 0;
    for (int c = 0; c < ncpu && c < 64; c++) {
        bool fast = best > 0 ? freq[c] * 4 >= best * 3 : c >= ncpu / 2;
        if (!fast) continue;
        if (skipped < skip_first) { skipped++; continue; }
        CPU_SET(c, &set); chosen++;
    }
    return chosen;
}

// Moves the calling thread to the root cpuset (the service itself stays restricted) and pins it
// to the fast cores. Returns how many cores were pinned, or -1 if pinning failed.
static int pin_fast(int skip_first = 0) {
    int tid = int(syscall(SYS_gettid));
    int cs = open("/dev/cpuset/tasks", O_WRONLY | O_CLOEXEC);
    if (cs >= 0) { dprintf(cs, "%d\n", tid); close(cs); }
    cpu_set_t set; int n = fast_cores(set, skip_first);
    if (n == 0) n = fast_cores(set, 0);
    return n > 0 && sched_setaffinity(tid, sizeof(set), &set) == 0 ? n : -1;
}
}
