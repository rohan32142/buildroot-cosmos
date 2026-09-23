/*
 * pldrift - log PL counter against CPU clocks for drift measurement.
 *
 * Once per interval:
 *   1. Coarse scan: copy the low timestamp word of every OCM record and find
 *      the write head (the one place where the delta is not the expected
 *      value). Retries if the snapshot is inconsistent.
 *   2. Fine read: bracketed by clock reads, walk forward from the head while
 *      the delta is still the expected value, to reach the newest record.
 *   3. Read that record's 64-bit timestamp (hi/lo/hi to reject torn reads).
 *
 * Output CSV (one row per sample):
 *   mono_raw_ns,realtime_ns,pl64,pl_lo,scan_ns,temp_mc,retries
 *
 * Record layout assumed: 8-byte little-endian timestamp at offset 0
 * (low word at +0, high word at +4), stride 32 bytes.
 *
 * Record-to-record deltas are 10000 or 10001 ticks, not a constant 10000:
 * the carrier period averages 10000.017 ticks (1.699 ppm phase-increment
 * deficit), so a delta within +/-1 tick of the expected value is accepted.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define REC_STRIDE   32u
#define MAX_RECORDS  8192u
#define FINE_LIMIT   256u
#define MAX_RETRIES  5
#define MAX_BRK_LOG  4u

static volatile sig_atomic_t stop;
static void on_signal(int s) { (void)s; stop = 1; }

static uint64_t now_ns(clockid_t c)
{
    struct timespec ts;
    clock_gettime(c, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline uint32_t rd32(volatile uint8_t *base, uint32_t idx, uint32_t off)
{
    return *(volatile uint32_t *)(base + idx * REC_STRIDE + off);
}

/* True if d is within +/-1 tick of the expected delta. */
static inline int is_step(uint32_t d, uint32_t delta)
{
    return (uint32_t)(d - delta + 1u) <= 2u;
}

/* Returns head index (newest record in the snapshot) or -1 if the snapshot
 * does not contain exactly one discontinuity. Reports the break count in *nb
 * and the first MAX_BRK_LOG break indices in brk[]. */
static int find_head(volatile uint8_t *ring, uint32_t n, uint32_t delta,
                     uint32_t *snap, uint32_t *nb, uint32_t *brk)
{
    uint32_t i, breaks = 0;
    int head = -1;

    for (i = 0; i < n; i++)
        snap[i] = rd32(ring, i, 0);

    for (i = 0; i < n; i++) {
        uint32_t next = snap[(i + 1) % n];
        if (!is_step(next - snap[i], delta)) {
            if (breaks < MAX_BRK_LOG) brk[breaks] = i;
            breaks++;
            head = (int)i;
        }
    }
    *nb = breaks;
    return breaks == 1 ? head : -1;
}

/* Temperature in milli-degC from an IIO device, or INT32_MIN if unavailable. */
struct temp_src { char raw[256]; double offset, scale; int ok; };

static int read_double(const char *path, double *v)
{
    FILE *f = fopen(path, "r");
    int r;
    if (!f) return -1;
    r = fscanf(f, "%lf", v);
    fclose(f);
    return r == 1 ? 0 : -1;
}

static void temp_init(struct temp_src *t, const char *dir)
{
    char p[256];
    t->ok = 0;
    if (!dir) return;
    snprintf(t->raw, sizeof t->raw, "%s/in_temp0_raw", dir);
    snprintf(p, sizeof p, "%s/in_temp0_offset", dir);
    if (read_double(p, &t->offset)) return;
    snprintf(p, sizeof p, "%s/in_temp0_scale", dir);
    if (read_double(p, &t->scale)) return;
    t->ok = 1;
}

static int32_t temp_read(struct temp_src *t)
{
    double raw;
    if (!t->ok || read_double(t->raw, &raw)) return INT32_MIN;
    return (int32_t)((raw + t->offset) * t->scale); /* IIO scale gives mC */
}

static void usage(const char *p)
{
    fprintf(stderr,
        "usage: %s -o out.csv [options]\n"
        "  -o FILE   output CSV (use /mnt/sd/...)\n"
        "  -i SEC    sample interval, default 1\n"
        "  -d SEC    duration, 0 = until Ctrl-C (default 0)\n"
        "  -b ADDR   ring base address, default 0xFFFC0000\n"
        "  -n N      records in ring, default 2048\n"
        "  -e TICKS  expected delta between records (+/-1 accepted), default 10000\n"
        "  -m DEV    memory device or file, default /dev/mem\n"
        "  -T DIR    IIO dir for temperature, e.g. /sys/bus/iio/devices/iio:device0\n"
        "  -t TAG    free-text tag written to the header (session id etc.)\n", p);
}

int main(int argc, char **argv)
{
    const char *out = NULL, *dev = "/dev/mem", *tdir = NULL, *tag = "";
    unsigned long base = 0xFFFC0000ul;
    uint32_t n = 2048, delta = 10000;
    double interval = 1.0, duration = 0.0;
    static uint32_t snap[MAX_RECORDS];
    struct temp_src temp;
    struct timespec next;
    uint64_t start_raw, rows = 0, rejects = 0;
    long pg = sysconf(_SC_PAGESIZE);
    int c, fd;
    FILE *f;

    while ((c = getopt(argc, argv, "o:i:d:b:n:e:m:T:t:h")) != -1) {
        switch (c) {
        case 'o': out = optarg; break;
        case 'i': interval = atof(optarg); break;
        case 'd': duration = atof(optarg); break;
        case 'b': base = strtoul(optarg, NULL, 0); break;
        case 'n': n = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'e': delta = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'm': dev = optarg; break;
        case 'T': tdir = optarg; break;
        case 't': tag = optarg; break;
        default: usage(argv[0]); return 2;
        }
    }
    if (!out || n < 2 || n > MAX_RECORDS || interval <= 0) {
        usage(argv[0]);
        return 2;
    }

    fd = open(dev, O_RDONLY | O_SYNC);
    if (fd < 0) { perror(dev); return 1; }

    unsigned long map_base = base & ~(unsigned long)(pg - 1);
    size_t map_off = base - map_base;
    size_t map_len = map_off + (size_t)n * REC_STRIDE;
    uint8_t *map = mmap(NULL, map_len, PROT_READ, MAP_SHARED, fd, (off_t)map_base);
    if (map == MAP_FAILED) { perror("mmap"); return 1; }
    volatile uint8_t *ring = map + map_off;

    f = fopen(out, "a");
    if (!f) { perror(out); return 1; }

    temp_init(&temp, tdir);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    fprintf(f, "# pldrift v1 tag=%s base=0x%lX n=%u delta=%u(+/-1) interval=%.3f temp=%s\n",
            tag, base, n, delta, interval, temp.ok ? tdir : "none");
    fprintf(f, "mono_raw_ns,realtime_ns,pl64,pl_lo,scan_ns,temp_mc,retries\n");
    fflush(f);

    start_raw = now_ns(CLOCK_MONOTONIC_RAW);
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (!stop) {
        int head = -1, tries;
        uint32_t nb = 0, brk[MAX_BRK_LOG], k;

        for (tries = 0; tries < MAX_RETRIES && head < 0; tries++)
            head = find_head(ring, n, delta, snap, &nb, brk);

        if (head < 0) {
            rejects++;
            fprintf(stderr, "pldrift: no consistent head after %d tries, %u breaks:",
                    MAX_RETRIES, nb);
            for (k = 0; k < nb && k < MAX_BRK_LOG; k++)
                fprintf(stderr, " [%u] %08X->%08X", brk[k], snap[brk[k]],
                        snap[(brk[k] + 1) % n]);
            fputc('\n', stderr);
        } else {
            uint64_t r0 = now_ns(CLOCK_MONOTONIC_RAW);
            uint64_t w0 = now_ns(CLOCK_REALTIME);

            /* Walk forward to the true newest record. */
            uint32_t idx = (uint32_t)head, steps = 0;
            uint32_t cur = rd32(ring, idx, 0);
            while (steps < FINE_LIMIT) {
                uint32_t nidx = (idx + 1) % n;
                uint32_t nv = rd32(ring, nidx, 0);
                if (!is_step(nv - cur, delta)) break;
                idx = nidx; cur = nv; steps++;
            }
            uint32_t hi1 = rd32(ring, idx, 4);
            uint32_t lo  = rd32(ring, idx, 0);
            uint32_t hi2 = rd32(ring, idx, 4);

            uint64_t w1 = now_ns(CLOCK_REALTIME);
            uint64_t r1 = now_ns(CLOCK_MONOTONIC_RAW);

            if (hi1 != hi2 || steps >= FINE_LIMIT) {
                rejects++;
            } else {
                uint64_t pl64 = ((uint64_t)hi1 << 32) | lo;
                fprintf(f, "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu32
                           ",%" PRIu64 ",%" PRId32 ",%d\n",
                        r0 + (r1 - r0) / 2, w0 + (w1 - w0) / 2,
                        pl64, lo, r1 - r0, temp_read(&temp), tries - 1);
                rows++;
                if (rows % 10 == 0) { fflush(f); fsync(fileno(f)); }
            }
        }

        if (duration > 0 &&
            (now_ns(CLOCK_MONOTONIC_RAW) - start_raw) / 1e9 >= duration)
            break;

        uint64_t step = (uint64_t)(interval * 1e9);
        next.tv_nsec += (long)(step % 1000000000ull);
        next.tv_sec  += (time_t)(step / 1000000000ull);
        if (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR && !stop)
            ;
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);
    fprintf(stderr, "pldrift: %" PRIu64 " rows, %" PRIu64 " rejects\n", rows, rejects);
    munmap(map, map_len);
    close(fd);
    return 0;
}