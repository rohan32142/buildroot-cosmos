/*
 * ocmsoak - OCM ring drain soak test for the PHiLICS Cosmos XZQ10.
 *
 * Follows the PL OCM ring (2,048 records, 32-byte stride, 64-bit PL
 * timestamp at offset 0, six int16 channels at 0x08..0x13) the same way the
 * telemetry daemon does, and measures whether the reader keeps up.
 *
 * Per report interval it logs: records consumed, records missed (from PL
 * timestamp gaps), gap events, torn reads, unexpected deltas, the longest
 * time between two drain passes, the largest number of records waiting in
 * one pass, and mean / AC RMS of one channel (sine sanity check).
 *
 * Loss rule: consecutive records are 10,000 or 10,001 ticks apart, so
 *   missed = llround(delta / 10000.017) - 1   (measured mean spacing)
 * When the reader falls more than one ring pass (102.4 ms) behind, the PL
 * has overwritten the slot it expects next; the reader takes the newer
 * record in that slot, so loss from a lap shows as exactly 2,048 records.
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define OCM_BASE      0xFFFC0000ULL
#define NREC          2048u
#define STRIDE        32u
#define RING_BYTES    (NREC * STRIDE)
#define NCH           6
#define TICKS_PER_REC 10000.017   /* measured 19,999.966 Hz */
#define TICK_HZ       200000000.0
#define REC_HZ        (TICK_HZ / TICKS_PER_REC)
#define RING_NS       ((double)NREC / REC_HZ * 1e9)

static volatile sig_atomic_t stop;
static volatile uint32_t *ring;

static void on_sig(int s) { (void)s; stop = 1; }

static inline uint64_t now_ns(clockid_t c)
{
    struct timespec t;
    clock_gettime(c, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void nap_ns(uint64_t ns)
{
    struct timespec t = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) };
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &t, &t) == EINTR && !stop) {}
}

/* 64-bit timestamp from two 32-bit loads, guarded against a carry between them. */
static inline uint64_t read_ts(unsigned i)
{
    volatile uint32_t *p = ring + i * (STRIDE / 4);
    uint32_t hi, lo, hi2;
    do { hi = p[1]; lo = p[0]; hi2 = p[1]; } while (hi != hi2);
    return ((uint64_t)hi << 32) | lo;
}

/* Channel c: words at 0x08, 0x0C, 0x10; low half first. 12-bit left-justified. */
static inline int ch_code(const uint32_t w[3], int c)
{
    uint16_t raw = (uint16_t)(w[c / 2] >> ((c & 1) ? 16 : 0));
    return (int16_t)raw >> 4;
}

/* Histogram of time between drain passes, whole run. Upper edges in ms. */
static const double hist_edge_ms[] = { 1, 2, 5, 10, 25, 50, 75, 100 };
#define NHIST (sizeof hist_edge_ms / sizeof hist_edge_ms[0] + 1)

struct win {
    uint64_t records, missed, gaps, torn, bad;
    uint64_t max_iter_ns;
    unsigned max_batch;
    double sum, sumsq;
    uint64_t n;
};

static void usage(void)
{
    fprintf(stderr,
        "usage: ocmsoak [options]\n"
        "  -d SEC    duration (0 = until Ctrl+C, default 0)\n"
        "  -i SEC    report interval (default 1)\n"
        "  -p US     poll period between drain passes (default 1000)\n"
        "  -c CH     channel for mean/AC RMS column, 0-5 (default 0)\n"
        "  -f PRIO   run as SCHED_FIFO at PRIO (1-99), with mlockall\n"
        "  -j MS     inject a stall of MS ms ...\n"
        "  -J SEC    ... every SEC seconds (default 10)\n"
        "  -o FILE   also write the log to FILE\n"
        "  -t TAG    free-text tag written to the header\n"
        "  -m PATH   memory device (default /dev/mem)\n"
        "  -a ADDR   ring physical address (default 0xFFFC0000)\n");
}

int main(int argc, char **argv)
{
    double dur = 0, interval = 1, stall_period = 10;
    long poll_us = 1000, stall_ms = 0;
    int chsel = 0, prio = 0;
    const char *out = NULL, *tag = "", *memdev = "/dev/mem";
    uint64_t base = OCM_BASE;
    int opt;

    while ((opt = getopt(argc, argv, "d:i:p:c:f:j:J:o:t:m:a:h")) != -1) {
        switch (opt) {
        case 'd': dur = atof(optarg); break;
        case 'i': interval = atof(optarg); break;
        case 'p': poll_us = atol(optarg); break;
        case 'c': chsel = atoi(optarg); break;
        case 'f': prio = atoi(optarg); break;
        case 'j': stall_ms = atol(optarg); break;
        case 'J': stall_period = atof(optarg); break;
        case 'o': out = optarg; break;
        case 't': tag = optarg; break;
        case 'm': memdev = optarg; break;
        case 'a': base = strtoull(optarg, NULL, 0); break;
        default: usage(); return opt == 'h' ? 0 : 2;
        }
    }
    if (chsel < 0 || chsel >= NCH || interval <= 0 || poll_us < 0) { usage(); return 2; }

    int fd = open(memdev, O_RDONLY | O_SYNC);
    if (fd < 0) { perror(memdev); return 1; }
    void *m = mmap(NULL, RING_BYTES, PROT_READ, MAP_SHARED, fd, (off_t)base);
    if (m == MAP_FAILED) { perror("mmap"); return 1; }
    ring = (volatile uint32_t *)m;

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    if (prio > 0) {
        struct sched_param sp = { .sched_priority = prio };
        if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) { perror("sched_setscheduler"); return 1; }
        if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) perror("mlockall (continuing)");
    }

    /* Find the write head: the slot with the largest timestamp. Check it moves. */
    unsigned head = 0;
    uint64_t head_ts = 0;
    for (unsigned i = 0; i < NREC; i++) {
        uint64_t t = read_ts(i);
        if (t > head_ts) { head_ts = t; head = i; }
    }
    nap_ns(250000000ull);
    uint64_t newest = 0;
    unsigned newest_i = 0;
    for (unsigned i = 0; i < NREC; i++) {
        uint64_t t = read_ts(i);
        if (t > newest) { newest = t; newest_i = i; }
    }
    if (head_ts == 0 || newest <= head_ts) {
        fprintf(stderr, "ocmsoak: ring not advancing; is the DAQ started? (adcmon start)\n");
        return 1;
    }
    /* The ring has lapped during the wait, so start from the fresh head. */
    head = newest_i;
    head_ts = newest;
    /* Start just behind the head so the first pass sees normal deltas. */
    unsigned idx = (head + 1) % NREC;
    uint64_t last_ts = head_ts;
    const uint64_t first_ts = head_ts;

    FILE *fo = NULL;
    if (out && !(fo = fopen(out, "w"))) { perror(out); return 1; }
    FILE *outs[2] = { stdout, fo };
    setvbuf(stdout, NULL, _IOLBF, 0);

    uint64_t rt0 = now_ns(CLOCK_REALTIME);
    for (int k = 0; k < 2; k++) {
        if (!outs[k]) continue;
        fprintf(outs[k], "# ocmsoak v1 tag=%s start_realtime_ns=%" PRIu64 " start_pl=%" PRIu64 "\n",
                tag, rt0, first_ts);
        fprintf(outs[k], "# poll_us=%ld interval_s=%g sched=%s prio=%d stall_ms=%ld stall_period_s=%g ch=%d ring_ms=%.1f\n",
                poll_us, interval, prio ? "FIFO" : "OTHER", prio, stall_ms, stall_period, chsel, RING_NS / 1e6);
        fprintf(outs[k], "t_s,records,missed,gap_events,torn,bad_delta,max_iter_ms,max_batch,ch_mean,ch_acrms\n");
    }

    struct win w = { 0 }, tot = { 0 };
    uint64_t hist[NHIST] = { 0 };
    uint64_t t0 = now_ns(CLOCK_MONOTONIC), t_prev = t0;
    uint64_t t_report = t0 + (uint64_t)(interval * 1e9);
    uint64_t t_stall = t0 + (uint64_t)(stall_period * 1e9);
    uint64_t t_end = dur > 0 ? t0 + (uint64_t)(dur * 1e9) : 0;
    uint64_t stalls = 0;

    while (!stop) {
        uint64_t t = now_ns(CLOCK_MONOTONIC);
        uint64_t it = t - t_prev;
        t_prev = t;
        if (it > w.max_iter_ns) w.max_iter_ns = it;
        double it_ms = it / 1e6;
        unsigned h = 0;
        while (h < NHIST - 1 && it_ms >= hist_edge_ms[h]) h++;
        hist[h]++;

        unsigned batch = 0;
        while (batch < NREC) {
            uint64_t ts = read_ts(idx);
            if (ts <= last_ts) break;                    /* not written yet */
            volatile uint32_t *p = ring + idx * (STRIDE / 4);
            uint32_t d[3] = { p[2], p[3], p[4] };
            if (read_ts(idx) != ts) { w.torn++; continue; } /* rewritten mid-read */

            int64_t delta = (int64_t)(ts - last_ts);
            long long k = llround((double)delta / TICKS_PER_REC);
            if (k > 1) { w.missed += (uint64_t)(k - 1); w.gaps++; }
            else if (delta != 10000 && delta != 10001) w.bad++;

            double v = ch_code(d, chsel);
            w.sum += v; w.sumsq += v * v; w.n++;
            last_ts = ts;
            idx = (idx + 1) % NREC;
            batch++;
            w.records++;
        }
        if (batch > w.max_batch) w.max_batch = batch;

        t = now_ns(CLOCK_MONOTONIC);
        if (t >= t_report) {
            double mean = w.n ? w.sum / w.n : 0;
            double var = w.n ? w.sumsq / w.n - mean * mean : 0;
            double ts_s = (t - t0) / 1e9;
            for (int k = 0; k < 2; k++)
                if (outs[k])
                    fprintf(outs[k], "%.3f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                                     ",%.3f,%u,%.2f,%.2f\n",
                            ts_s, w.records, w.missed, w.gaps, w.torn, w.bad,
                            w.max_iter_ns / 1e6, w.max_batch, mean, var > 0 ? sqrt(var) : 0);
            if (fo) fflush(fo);
            tot.records += w.records; tot.missed += w.missed; tot.gaps += w.gaps;
            tot.torn += w.torn; tot.bad += w.bad;
            if (w.max_iter_ns > tot.max_iter_ns) tot.max_iter_ns = w.max_iter_ns;
            if (w.max_batch > tot.max_batch) tot.max_batch = w.max_batch;
            memset(&w, 0, sizeof w);
            t_report += (uint64_t)(interval * 1e9);
        }
        if (t_end && t >= t_end) break;

        if (stall_ms > 0 && t >= t_stall) {
            nap_ns((uint64_t)stall_ms * 1000000ull);   /* simulated deschedule */
            stalls++;
            t_stall += (uint64_t)(stall_period * 1e9);
        }
        if (poll_us > 0) nap_ns((uint64_t)poll_us * 1000ull);
    }

    /* Fold in the partial last window. */
    tot.records += w.records; tot.missed += w.missed; tot.gaps += w.gaps;
    tot.torn += w.torn; tot.bad += w.bad;
    if (w.max_iter_ns > tot.max_iter_ns) tot.max_iter_ns = w.max_iter_ns;
    if (w.max_batch > tot.max_batch) tot.max_batch = w.max_batch;

    uint64_t t1 = now_ns(CLOCK_MONOTONIC);
    double elapsed = (t1 - t0) / 1e9;
    double span_rec = (double)(last_ts - first_ts) / TICKS_PER_REC;
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    double cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 +
                 ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
    double expect = tot.records + tot.missed;

    FILE *sums[2] = { stderr, fo };
    for (int k = 0; k < 2; k++) {
        FILE *s = sums[k];
        if (!s) continue;
        fprintf(s, "# summary elapsed_s=%.1f records=%" PRIu64 " missed=%" PRIu64 " gap_events=%" PRIu64
                   " torn=%" PRIu64 " bad_delta=%" PRIu64 "\n",
                elapsed, tot.records, tot.missed, tot.gaps, tot.torn, tot.bad);
        fprintf(s, "# summary loss_fraction=%.3g max_iter_ms=%.3f max_batch=%u peak_occupancy_pct=%.1f ring_ms=%.1f\n",
                expect > 0 ? tot.missed / expect : 0, tot.max_iter_ns / 1e6, tot.max_batch,
                100.0 * tot.max_batch / NREC, RING_NS / 1e6);
        fprintf(s, "# summary consistency records+missed=%.0f pl_span_records=%.0f stalls_injected=%" PRIu64
                   " cpu_pct=%.1f\n", expect, span_rec, stalls, elapsed > 0 ? 100 * cpu / elapsed : 0);
        fprintf(s, "# hist_iter_ms");
        for (unsigned b = 0; b < NHIST; b++) {
            if (b < NHIST - 1) fprintf(s, " <%g:%" PRIu64, hist_edge_ms[b], hist[b]);
            else fprintf(s, " >=%g:%" PRIu64, hist_edge_ms[b - 1], hist[b]);
        }
        fprintf(s, "\n");
    }
    if (fo) fclose(fo);
    munmap(m, RING_BYTES);
    close(fd);
    return tot.missed ? 3 : 0;
}