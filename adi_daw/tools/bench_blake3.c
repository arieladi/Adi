/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * BLAKE3 throughput, in memory: 1 GiB, five runs, the median in MiB/s
 * (ADR-0153). Built four ways by tools/bench_blake3.bat, so one run of the .bat
 * prints the portable, SSE4.1, AVX2 and AVX-512 numbers side by side. The
 * digest is printed so the four can be seen to agree.
 *
 * Portable C11: timespec_get, no platform headers.
 */
#include "blake3.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int cmp(const void* a, const void* b) {
    const double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

static double now(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

int main(void) {
    const size_t n = (size_t)1 << 30;
    unsigned char* buf = (unsigned char*)malloc(n);
    if (buf == NULL) return 1;
    for (size_t i = 0; i < n; ++i) buf[i] = (unsigned char)((i * 2654435761u) >> 24);
    double mibs[5];
    unsigned char out[32];
    for (int r = 0; r < 5; ++r) {
        const double t0 = now();
        blake3_hasher h;
        blake3_hasher_init(&h);
        blake3_hasher_update(&h, buf, n);
        blake3_hasher_finalize(&h, out, 32);
        mibs[r] = 1024.0 / (now() - t0);
    }
    qsort(mibs, 5, sizeof mibs[0], cmp);
    printf("median %.0f MiB/s  (min %.0f, max %.0f)  digest %02x%02x%02x%02x\n",
           mibs[2], mibs[0], mibs[4], out[0], out[1], out[2], out[3]);
    free(buf);
    return 0;
}
