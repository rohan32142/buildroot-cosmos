/*
 * adcmon - ADC channel monitor for the PHiLICS Cosmos XZQ10 (bitstream 0xA3).
 *
 *   adcmon start                 check bitstream, run the DAQ start sequence,
 *                                confirm the OCM ring is advancing
 *   adcmon live   [-c 0,1] [-i SEC] [-d SEC]
 *                                per-channel mean/min/max/p-p/AC RMS/frequency
 *                                over each interval, from every sample
 *   adcmon stream [-c 0] [-r LINES_PER_S] [-d SEC]
 *                                one sample every so often (slow signals only)
 *   adcmon record -o FILE [-d SEC] [-t TAG]
 *                                every sample to CSV (write to /tmp)
 *
 * Common options: -g CONF (gains, default /mnt/sd/adcmon.conf), -m DEV
 * (default /dev/mem), -p US (poll period, default 1000).
 *
 * Hardware facts used here (measured Sep 2026):
 *   OCM ring 0xFFFC0000, 2,048 records, 32-byte stride.
 *   Record: u64 PL timestamp (200 MHz) at +0x00; six 16-bit channels at
 *   +0x08..+0x13, low half of each word first; each is a 12-bit two's
 *   complement code left-justified (value = (int16)raw >> 4).
 *   Record delta 10,000 or 10,001 ticks (19,999.966 Hz).
 *   Start: 16-bit params 0x4000, 0x0000, 0x46DC, 0x0003 to 0xFFFFF800..806,
 *   then 1 to the strobe at 0x80000010. Revision register 0x80000004.
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define RING_BASE     0xFFFC0000ULL
#define NREC          2048u
#define STRIDE        32u
#define PARAM_BASE    0xFFFFF800ULL
#define PL_REV        0x80000004ULL
#define PL_STROBE     0x80000010ULL
#define REV_EXPECTED  0xA3u
#define NCH           6
#define TICK_HZ       200000000.0
#define TICKS_PER_REC 10000.017   /* measured 19,999.966 Hz */
#define REC_HZ        (TICK_HZ / TICKS_PER_REC)

static const char *ch_name[NCH]  = { "I_F", "I_L", "V_C", "V_R", "V_DCP", "V_DCN" };
static const char *ch_input[NCH] = { "A23-A0", "A23-B0", "A45-A0", "A45-B0", "A45-C0", "A45-A1" };
static const uint16_t start_params[4] = { 0x4000, 0x0000, 0x46DC, 0x0003 };

static volatile sig_atomic_t stop;
static void on_signal(int s) { (void)s; stop = 1; }

struct opts {
    const char *dev, *conf, *out, *tag;
    unsigned chmask;
    double interval, duration, rate;
    long poll_us;
    double gain[NCH];          /* codes per volt; 0 = show codes */
};

struct rec { uint64_t ts; int code[NCH]; };

struct ring {
    volatile uint32_t *p;
    unsigned idx;              /* next slot to read */
    uint64_t last;             /* timestamp of last record consumed */
    uint64_t overruns;         /* gap events */
    uint64_t missed;           /* records lost */
};

/* ------------------------------------------------------------------ util */

static uint64_t now_ns(clockid_t c)
{
    struct timespec t;
    clock_gettime(c, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void nap(long us)
{
    if (us <= 0) return;
    struct timespec t = { us / 1000000, (us % 1000000) * 1000 };
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &t, &t) == EINTR && !stop) {}
}

static void *map_phys(const char *dev, uint64_t addr, size_t len, int prot, int flags_rw)
{
    int fd = open(dev, (flags_rw ? O_RDWR : O_RDONLY) | O_SYNC);
    if (fd < 0) { perror(dev); return NULL; }
    long pg = sysconf(_SC_PAGESIZE);
    uint64_t base = addr & ~(uint64_t)(pg - 1);
    size_t off = (size_t)(addr - base);
    void *m = mmap(NULL, len + off, prot, MAP_SHARED, fd, (off_t)base);
    close(fd);
    if (m == MAP_FAILED) { perror("mmap"); return NULL; }
    return (char *)m + off;
}

static void load_gains(struct opts *o)
{
    FILE *f = fopen(o->conf, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        int c; double g;
        if (line[0] == '#') continue;
        if (sscanf(line, " ch%d %lf", &c, &g) == 2 && c >= 0 && c < NCH && g > 0)
            o->gain[c] = g;
    }
    fclose(f);
}

static unsigned parse_mask(const char *s)
{
    unsigned m = 0;
    char *dup = strdup(s), *tok, *save = NULL;
    for (tok = strtok_r(dup, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        int c = atoi(tok);
        if (c >= 0 && c < NCH) m |= 1u << c;
    }
    free(dup);
    return m ? m : 0x3F;
}

static const char *unit(const struct opts *o, int c) { return o->gain[c] > 0 ? "V" : "code"; }
static double scale(const struct opts *o, int c, double v) { return o->gain[c] > 0 ? v / o->gain[c] : v; }

/* ------------------------------------------------------------------ ring */

static uint64_t read_ts(struct ring *r, unsigned i)
{
    volatile uint32_t *q = r->p + i * (STRIDE / 4);
    uint32_t hi, lo, hi2;
    do { hi = q[1]; lo = q[0]; hi2 = q[1]; } while (hi != hi2);
    return ((uint64_t)hi << 32) | lo;
}

static int newest_slot(struct ring *r, uint64_t *ts)
{
    unsigned best = 0; uint64_t bt = 0;
    for (unsigned i = 0; i < NREC; i++) {
        uint64_t t = read_ts(r, i);
        if (t > bt) { bt = t; best = i; }
    }
    *ts = bt;
    return (int)best;
}

/* Find the write head and confirm it moves. Returns 0 on success. */
static int ring_attach(struct ring *r)
{
    uint64_t t0, t1;
    newest_slot(r, &t0);
    nap(250000);
    int h = newest_slot(r, &t1);
    if (t0 == 0 || t1 <= t0) return -1;
    r->idx = (unsigned)(h + 1) % NREC;
    r->last = t1;
    return 0;
}

/* 1 = record returned, 0 = nothing new yet. Counts gaps as overruns. */
static int ring_next(struct ring *r, struct rec *out)
{
    for (;;) {
        uint64_t ts = read_ts(r, r->idx);
        if (ts <= r->last) return 0;
        volatile uint32_t *q = r->p + r->idx * (STRIDE / 4);
        uint32_t w[3] = { q[2], q[3], q[4] };
        if (read_ts(r, r->idx) != ts) continue;          /* rewritten mid-read */
        long long k = llround((double)(ts - r->last) / TICKS_PER_REC);
        if (k > 1) { r->overruns++; r->missed += (uint64_t)(k - 1); }
        out->ts = ts;
        for (int c = 0; c < NCH; c++)
            out->code[c] = (int16_t)(uint16_t)(w[c / 2] >> ((c & 1) ? 16 : 0)) >> 4;
        r->last = ts;
        r->idx = (r->idx + 1) % NREC;
        return 1;
    }
}

/* ----------------------------------------------------------------- start */

static int cmd_start(struct opts *o, struct ring *r)
{
    volatile uint32_t *pl = map_phys(o->dev, PL_REV & ~0xFFFull, 0x1000, PROT_READ | PROT_WRITE, 1);
    volatile uint16_t *par = map_phys(o->dev, PARAM_BASE, 8, PROT_READ | PROT_WRITE, 1);
    if (!pl || !par) return 1;

    uint32_t rev = pl[(PL_REV & 0xFFF) / 4];
    if ((rev & 0xFF) != REV_EXPECTED) {
        fprintf(stderr, "adcmon: bitstream revision 0x%X, expected 0x%X; not starting\n",
                rev, REV_EXPECTED);
        return 1;
    }
    for (int i = 0; i < 4; i++) par[i] = start_params[i];
    __sync_synchronize();
    pl[(PL_STROBE & 0xFFF) / 4] = 1;
    __sync_synchronize();

    nap(100000);
    if (ring_attach(r) < 0) {
        fprintf(stderr, "adcmon: start sequence written but the ring is not advancing\n");
        return 1;
    }
    struct rec a, b;
    uint64_t t_end = now_ns(CLOCK_MONOTONIC) + 500000000ull;
    int got = 0;
    while (got < 2 && now_ns(CLOCK_MONOTONIC) < t_end) {
        if (ring_next(r, got ? &b : &a) == 1) got++;
        else nap(1000);
    }
    if (got < 2) {
        fprintf(stderr, "adcmon: ring not advancing after start\n");
        return 1;
    }
    printf("adcmon: DAQ running (revision 0x%X, record delta %" PRIu64 " ticks, PL counter %" PRIu64 ")\n",
           rev & 0xFF, b.ts - a.ts, b.ts);
    return 0;
}

/* ------------------------------------------------------------------ live */

/* Frequency from rising crossings of the mean, with hysteresis, linear
 * interpolation between samples, timed by PL timestamps. */
static double est_freq(const uint64_t *ts, const int16_t *x, size_t n, double mean, double pp)
{
    if (pp < 8) return NAN;
    double hyst = pp * 0.1;
    int armed = 0, have = 0;
    double first = 0, last = 0; long cross = 0;
    for (size_t i = 1; i < n; i++) {
        double v0 = x[i - 1] - mean, v1 = x[i] - mean;
        if (v1 < -hyst) armed = 1;
        if (armed && v0 < 0 && v1 >= 0) {
            double frac = v0 / (v0 - v1);
            double t = (double)ts[i - 1] + frac * (double)(ts[i] - ts[i - 1]);
            if (!have) { first = t; have = 1; } else { last = t; cross++; }
            armed = 0;
        }
    }
    if (cross < 1) return NAN;
    return cross / ((last - first) / TICK_HZ);
}

static int cmd_live(struct opts *o, struct ring *r)
{
    size_t cap = (size_t)(o->interval * REC_HZ * 1.1) + 4096;
    uint64_t *ts = malloc(cap * sizeof *ts);
    int16_t *x[NCH];
    for (int c = 0; c < NCH; c++) x[c] = malloc(cap * sizeof(int16_t));
    if (!ts) { perror("malloc"); return 1; }

    uint64_t t_end = o->duration > 0 ? now_ns(CLOCK_MONOTONIC) + (uint64_t)(o->duration * 1e9) : 0;
    uint64_t ovr0 = r->overruns, miss0 = r->missed;
    uint64_t start_ts = r->last;
    struct rec rec;
    setvbuf(stdout, NULL, _IOLBF, 0);

    while (!stop && (!t_end || now_ns(CLOCK_MONOTONIC) < t_end)) {
        size_t n = 0;
        uint64_t t_int = now_ns(CLOCK_MONOTONIC) + (uint64_t)(o->interval * 1e9);
        while (!stop && now_ns(CLOCK_MONOTONIC) < t_int) {
            while (n < cap && ring_next(r, &rec) == 1) {
                ts[n] = rec.ts;
                for (int c = 0; c < NCH; c++) x[c][n] = (int16_t)rec.code[c];
                n++;
            }
            nap(o->poll_us);
        }
        if (n < 2) continue;

        printf("\n t = %.1f s   %zu samples\n", (double)(ts[n - 1] - start_ts) / TICK_HZ, n);
        if (r->overruns > ovr0) {
            printf(" WARNING: overrun, %" PRIu64 " samples missed\n", r->missed - miss0);
            ovr0 = r->overruns; miss0 = r->missed;
        }
        printf("  ch  signal  input    unit      mean      min      max      p-p   AC rms    freq Hz\n");
        for (int c = 0; c < NCH; c++) {
            if (!(o->chmask & (1u << c))) continue;
            double sum = 0, sq = 0; int mn = 32767, mx = -32768;
            for (size_t i = 0; i < n; i++) {
                int v = x[c][i];
                sum += v; sq += (double)v * v;
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            double mean = sum / n;
            double var = sq / n - mean * mean;
            double rms = var > 0 ? sqrt(var) : 0;
            double f = est_freq(ts, x[c], n, mean, mx - mn);
            printf("  %2d  %-6s  %-7s  %-4s  %8.3f %8.3f %8.3f %8.3f %8.3f  ",
                   c, ch_name[c], ch_input[c], unit(o, c),
                   scale(o, c, mean), scale(o, c, mn), scale(o, c, mx),
                   scale(o, c, mx - mn), scale(o, c, rms));
            if (isnan(f)) printf("%9s\n", "-");
            else printf("%9.3f\n", f);
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- stream */

static int cmd_stream(struct opts *o, struct ring *r)
{
    long dec = lround(REC_HZ / (o->rate > 0 ? o->rate : 20));
    if (dec < 1) dec = 1;
    uint64_t t_end = o->duration > 0 ? now_ns(CLOCK_MONOTONIC) + (uint64_t)(o->duration * 1e9) : 0;
    uint64_t ovr0 = r->overruns, start = r->last;
    long count = 0;
    struct rec rec;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("# adcmon stream: every %ld samples (%.1f lines/s)\n", dec, REC_HZ / dec);
    printf("%10s", "t (s)");
    for (int c = 0; c < NCH; c++)
        if (o->chmask & (1u << c))
            printf("  %8s(%s)", ch_name[c], o->gain[c] > 0 ? "V" : "c");
    printf("\n");

    while (!stop && (!t_end || now_ns(CLOCK_MONOTONIC) < t_end)) {
        while (ring_next(r, &rec) == 1) {
            if (r->overruns > ovr0) { printf("# overrun: samples missed\n"); ovr0 = r->overruns; }
            if (count++ % dec) continue;
            printf("%10.4f", (double)(rec.ts - start) / TICK_HZ);
            for (int c = 0; c < NCH; c++)
                if (o->chmask & (1u << c)) {
                    if (o->gain[c] > 0) printf("  %11.4f", rec.code[c] / o->gain[c]);
                    else printf("  %11d", rec.code[c]);
                }
            printf("\n");
        }
        nap(o->poll_us);
    }
    return 0;
}

/* ---------------------------------------------------------------- record */

static int cmd_record(struct opts *o, struct ring *r)
{
    if (!o->out) { fprintf(stderr, "adcmon: record needs -o FILE\n"); return 2; }
    if (!strncmp(o->out, "/mnt/sd", 7))
        fprintf(stderr, "adcmon: warning: the SD card is too slow for 20 kHz; use /tmp\n");
    FILE *f = fopen(o->out, "w");
    if (!f) { perror(o->out); return 1; }
    setvbuf(f, NULL, _IOFBF, 1 << 20);

    double dur = o->duration > 0 ? o->duration : 5;
    fprintf(f, "# adcmon record v1 tag=%s n=%u delta=10000(+/-1) anchor_pl=%" PRIu64
               " anchor_realtime_ns=%" PRIu64 "\n", o->tag, NREC, r->last, now_ns(CLOCK_REALTIME));
    fprintf(f, "# gains_codes_per_volt=");
    for (int c = 0; c < NCH; c++) fprintf(f, "%s%g", c ? "," : "", o->gain[c]);
    fprintf(f, "\n# values are ADC codes (raw >> 4)\npl64");
    for (int c = 0; c < NCH; c++) fprintf(f, ",ch%d_%s", c, ch_name[c]);
    fprintf(f, "\n");

    uint64_t t_end = now_ns(CLOCK_MONOTONIC) + (uint64_t)(dur * 1e9);
    uint64_t rows = 0, first = 0, ovr0 = r->overruns, miss0 = r->missed;
    struct rec rec;
    while (!stop && now_ns(CLOCK_MONOTONIC) < t_end) {
        while (ring_next(r, &rec) == 1) {
            if (r->overruns > ovr0) {
                fprintf(f, "# overrun: resync before pl=%" PRIu64 "\n", rec.ts);
                ovr0 = r->overruns;
            }
            fprintf(f, "%" PRIu64 ",%d,%d,%d,%d,%d,%d\n", rec.ts, rec.code[0], rec.code[1],
                    rec.code[2], rec.code[3], rec.code[4], rec.code[5]);
            if (!rows) first = rec.ts;
            rows++;
        }
        nap(o->poll_us);
    }
    fclose(f);

    double span = rows > 1 ? (double)(r->last - first) / TICK_HZ : 0;
    fprintf(stderr, "adcmon: %" PRIu64 " samples over %.3f s, %" PRIu64 " missed -> %s\n",
            rows, span, r->missed - miss0, o->out);
    if (rows > 1) fprintf(stderr, "adcmon: mean sample rate %.3f Hz\n", (rows - 1) / span);
    return r->missed > miss0 ? 3 : 0;
}

/* ------------------------------------------------------------------ main */

static void usage(void)
{
    fprintf(stderr,
        "usage: adcmon start\n"
        "       adcmon live   [-c 0,1] [-i SEC] [-d SEC]\n"
        "       adcmon stream [-c 0] [-r LINES_PER_S] [-d SEC]\n"
        "       adcmon record -o FILE [-d SEC] [-t TAG]\n"
        "options: -g CONF gains file (default /mnt/sd/adcmon.conf)\n"
        "         -m DEV  memory device (default /dev/mem)\n"
        "         -p US   poll period (default 1000)\n");
}

int main(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) { usage(); return argc < 2 ? 2 : 0; }
    const char *cmd = argv[1];
    struct opts o = { .dev = "/dev/mem", .conf = "/mnt/sd/adcmon.conf", .tag = "",
                      .chmask = 0x3F, .interval = 1, .rate = 20, .poll_us = 1000 };
    int opt;
    optind = 2;
    while ((opt = getopt(argc, argv, "c:i:d:r:o:t:g:m:p:h")) != -1) {
        switch (opt) {
        case 'c': o.chmask = parse_mask(optarg); break;
        case 'i': o.interval = atof(optarg); break;
        case 'd': o.duration = atof(optarg); break;
        case 'r': o.rate = atof(optarg); break;
        case 'o': o.out = optarg; break;
        case 't': o.tag = optarg; break;
        case 'g': o.conf = optarg; break;
        case 'm': o.dev = optarg; break;
        case 'p': o.poll_us = atol(optarg); break;
        default: usage(); return opt == 'h' ? 0 : 2;
        }
    }
    if (o.interval <= 0 || o.interval > 30) { fprintf(stderr, "adcmon: -i must be 0-30 s\n"); return 2; }
    load_gains(&o);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    struct ring r = { 0 };
    r.p = map_phys(o.dev, RING_BASE, NREC * STRIDE, PROT_READ, 0);
    if (!r.p) return 1;

    if (!strcmp(cmd, "start")) return cmd_start(&o, &r);

    if (ring_attach(&r) < 0) {
        fprintf(stderr, "adcmon: no consistent write head; is the DAQ running? (run: adcmon start)\n");
        return 1;
    }
    if (!strcmp(cmd, "live"))   return cmd_live(&o, &r);
    if (!strcmp(cmd, "stream")) return cmd_stream(&o, &r);
    if (!strcmp(cmd, "record")) return cmd_record(&o, &r);
    usage();
    return 2;
}