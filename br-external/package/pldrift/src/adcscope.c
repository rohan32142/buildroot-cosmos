/*
 * adcscope - live lab view of the PHiLICS ADC channels, written straight to
 * InfluxDB v2 as line protocol over HTTP.
 *
 *   adcscope [-c LIST] [-R CH] [-W V:I,...] [-r HZ] [-T PRE:POST] [-N]
 *            [-H HOST] [-P PORT] [-O ORG] [-b BUCKET] [-k TOKENFILE]
 *            [-n NODE] [-s SESSION] [-f MS] [-d SEC]
 *
 * Measurements written (tags node_id and session on every point):
 *   cycle       one point per 50 Hz cycle of the reference channel (-R):
 *               chN_mean, chN_rms (AC, about the cycle mean), chN_pp,
 *               freq, and for each V:I pair (-W) pV_I, qV_I, pfV_I.
 *               If the reference has no AC signal, 20 ms windows are used
 *               and freq/p/q/pf are left out. Also carries sts and src.
 *   wave        block-averaged waveform at -r Hz (default 1000), fields chN.
 *   trip        one point whenever tripsts or tripsrc changes: sts, src,
 *               prev_sts, and string fields active and cause with bit names.
 *   burst       full-rate capture around a trip (sts going non-zero) or a
 *               manual trigger (killall -USR1 adcscope), PRE s before and
 *               POST s after (-T, default 0.5:0.5). Same format as adcburst,
 *               with tag=trip or tag=manual.
 *   burst_meta  one summary point per capture.
 *   scope_meta  every 10 s: queue depth, drops, missed records, HTTP
 *               results, drain-thread wake-up lateness and loop time, and
 *               the timestamp offset. Also printed to stderr.
 * Values are in engineering units where /mnt/sd/adcmon.conf gives a gain
 * (codes per unit) for the channel, otherwise ADC codes.
 *
 * Threads:
 *   drain   SCHED_FIFO, wakes every -p us (default 1000) on an absolute
 *           schedule, copies new ring records into a 2^18-record queue
 *           (13 s), reads the trip registers every -t polls. No network,
 *           file or formatting work.
 *   worker  normal priority. Computes metrics, formats line protocol and
 *           POSTs every -f ms (default 500). If InfluxDB stalls the queue
 *           absorbs it; when full, records are dropped and counted.
 *
 * Timestamps: t_ns = pl * 5 + offset. After each drain, (CLOCK_REALTIME -
 * newest pl * 5) is the offset plus the age of that record. The drain
 * thread takes the minimum over each 1 s window and the worker uses the
 * smallest seen so far. A window minimum more than 1 ms above it (the
 * clock was stepped forward) replaces it.
 *
 * Trip registers (smartdac_regs.h): tripsts 0x8000002C, tripsrc 0x80000034,
 * low 32 bits. Bits: 0 I_F, 1 I_L, 2 V_C, 3 V_R, 4 V_DCP, 5 V_DCN,
 * 6 Watchdog, 7 eStop, 8 G3TGDI, 9 RTDS overflow. They are read only;
 * -N stops adcscope reading them at all.
 *
 * Run adcmon start (or labup) first. Stop with Ctrl+C.
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <netdb.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define RING_BASE     0xFFFC0000ULL
#define NREC          2048u
#define STRIDE        32u
#define PL_BASE       0x80000000ULL
#define TRIPSTS_OFF   0x2C
#define TRIPSRC_OFF   0x34
#define NCH           6
#define TICK_HZ       200000000.0
#define NS_PER_TICK   5
#define TICKS_PER_REC 10000.017
#define REC_HZ        (TICK_HZ / TICKS_PER_REC)
#define QBITS         18
#define QSIZE         (1u << QBITS)
#define QMASK         (QSIZE - 1)
#define WMAX          1200          /* longest cycle window, samples (~16.7 Hz) */
#define WFIX          400           /* window when there is no AC reference */
#define WAC_MAX       1000          /* close a cycle after this many (20 Hz) */
#define MINPP         8             /* codes; below this the reference is "no AC" */
#define MAXPAIR       6
#define NTRIPBIT      10

static const char *ch_name[NCH] = { "I_F", "I_L", "V_C", "V_R", "V_DCP", "V_DCN" };
static const char *trip_bit[NTRIPBIT] = {
    "I_F", "I_L", "V_C", "V_R", "V_DCP", "V_DCN", "Watchdog", "eStop", "G3TGDI", "RTDS_overflow" };

static volatile sig_atomic_t stop, trig_manual;
static void on_signal(int s) { if (s == SIGUSR1) trig_manual = 1; else stop = 1; }

/* ------------------------------------------------------------- settings */

struct opts {
    const char *dev, *conf, *host, *port, *org, *bucket, *tokfile, *node, *session;
    char *token;
    unsigned chmask;
    int ref;
    int npair, pv[MAXPAIR], pi[MAXPAIR];
    double wave_hz, pre_s, post_s, duration;
    long poll_us, flush_ms, trip_every;
    int no_trip, prio;
    double gain[NCH];
};
static struct opts o;

/* ---------------------------------------------------------------- util */

static uint64_t now_ns(clockid_t c)
{
    struct timespec t;
    clock_gettime(c, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void nap_us(long us)
{
    struct timespec t = { us / 1000000, (us % 1000000) * 1000 };
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &t, &t) == EINTR && !stop) {}
}

static void *map_phys(const char *dev, uint64_t addr, size_t len)
{
    int fd = open(dev, O_RDONLY | O_SYNC);
    if (fd < 0) { perror(dev); return NULL; }
    long pg = sysconf(_SC_PAGESIZE);
    uint64_t base = addr & ~(uint64_t)(pg - 1);
    size_t off = (size_t)(addr - base);
    void *m = mmap(NULL, len + off, PROT_READ, MAP_SHARED, fd, (off_t)base);
    close(fd);
    if (m == MAP_FAILED) { perror("mmap"); return NULL; }
    return (char *)m + off;
}

static void load_gains(void)
{
    FILE *f = fopen(o.conf, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        int c; double g;
        if (line[0] == '#') continue;
        if (sscanf(line, " ch%d %lf", &c, &g) == 2 && c >= 0 && c < NCH && g > 0)
            o.gain[c] = g;
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
    return m;
}

static int parse_pairs(const char *s)
{
    char *dup = strdup(s), *tok, *save = NULL;
    o.npair = 0;
    for (tok = strtok_r(dup, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        int v, i;
        if (sscanf(tok, "%d:%d", &v, &i) != 2 || v < 0 || v >= NCH || i < 0 || i >= NCH || v == i ||
            o.npair >= MAXPAIR) { free(dup); return -1; }
        o.pv[o.npair] = v; o.pi[o.npair] = i; o.npair++;
    }
    free(dup);
    return 0;
}

static char *tagval(const char *s)
{
    char *d = strdup(s && *s ? s : "none");
    for (char *p = d; *p; p++)
        if (*p == ' ' || *p == ',' || *p == '=' || *p == '"' || *p == '\\') *p = '_';
    return d;
}

static void urlenc(char *dst, size_t n, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (; *s && j + 4 < n; s++) {
        unsigned char ch = (unsigned char)*s;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~')
            dst[j++] = (char)ch;
        else { dst[j++] = '%'; dst[j++] = hex[ch >> 4]; dst[j++] = hex[ch & 15]; }
    }
    dst[j] = 0;
}

static char *read_token(void)
{
    const char *env = getenv("INFLUX_TOKEN");
    if (env && *env) return strdup(env);
    FILE *f = fopen(o.tokfile, "r");
    if (!f) return NULL;
    char buf[512] = { 0 };
    if (!fgets(buf, sizeof buf, f)) buf[0] = 0;
    fclose(f);
    buf[strcspn(buf, "\r\n \t")] = 0;
    return buf[0] ? strdup(buf) : NULL;
}

static double scl(int c, double v) { return o.gain[c] > 0 ? v / o.gain[c] : v; }

/* ---------------------------------------------------------------- ring */

struct rec { uint64_t ts; int16_t code[NCH]; uint32_t sts, src; };

struct ring {
    volatile uint32_t *p;
    unsigned idx;
    uint64_t last, missed;
};

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

static int ring_attach(struct ring *r)
{
    uint64_t t0, t1;
    newest_slot(r, &t0);
    nap_us(250000);
    int h = newest_slot(r, &t1);
    if (t0 == 0 || t1 <= t0) return -1;
    r->idx = (unsigned)(h + 1) % NREC;
    r->last = t1;
    return 0;
}

static int ring_next(struct ring *r, struct rec *out)
{
    for (;;) {
        uint64_t ts = read_ts(r, r->idx);
        if (ts <= r->last) return 0;
        volatile uint32_t *q = r->p + r->idx * (STRIDE / 4);
        uint32_t w[3] = { q[2], q[3], q[4] };
        if (read_ts(r, r->idx) != ts) continue;
        long long k = llround((double)(ts - r->last) / TICKS_PER_REC);
        if (k > 1) r->missed += (uint64_t)(k - 1);
        out->ts = ts;
        for (int c = 0; c < NCH; c++)
            out->code[c] = (int16_t)((int16_t)(uint16_t)(w[c / 2] >> ((c & 1) ? 16 : 0)) >> 4);
        r->last = ts;
        r->idx = (r->idx + 1) % NREC;
        return 1;
    }
}

/* ------------------------------------------------- shared drain state */

static struct ring ring;
static volatile uint32_t *plregs;
static struct rec *queue;
static uint32_t q_head, q_tail;             /* producer / consumer counters */

/* written by the drain thread, read by the worker (atomics) */
static uint64_t st_drop, st_missed, st_qmax, st_wake_max_ns, st_loop_max_ns, st_polls;
static int64_t  st_offset, st_win_spread;
static int      st_offset_valid;

static void *drain_main(void *arg)
{
    (void)arg;
    struct sched_param sp = { .sched_priority = o.prio };
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp))
        fprintf(stderr, "adcscope: SCHED_FIFO not set for the drain thread (%s)\n", strerror(errno));

    uint32_t sts = 0, src = 0;
    int64_t win_min = INT64_MAX, win_max = INT64_MIN, best = 0;
    int have_best = 0;
    uint64_t win_end = now_ns(CLOCK_MONOTONIC) + 1000000000ull;
    long n = 0;
    uint64_t period = (uint64_t)o.poll_us * 1000ull;
    uint64_t next = now_ns(CLOCK_MONOTONIC);

    while (!stop) {
        next += period;
        struct timespec ts = { (time_t)(next / 1000000000ull), (long)(next % 1000000000ull) };
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR && !stop) {}
        uint64_t woke = now_ns(CLOCK_MONOTONIC);
        uint64_t late = woke > next ? woke - next : 0;
        if (late > __atomic_load_n(&st_wake_max_ns, __ATOMIC_RELAXED))
            __atomic_store_n(&st_wake_max_ns, late, __ATOMIC_RELAXED);
        if (late > 10 * period) next = woke;          /* do not replay missed polls */

        if (plregs && (n++ % o.trip_every) == 0) {
            sts = plregs[TRIPSTS_OFF / 4];
            src = plregs[TRIPSRC_OFF / 4];
        }

        uint32_t head = q_head;
        uint32_t tail = __atomic_load_n(&q_tail, __ATOMIC_ACQUIRE);
        struct rec r;
        int got = 0;
        while (ring_next(&ring, &r)) {
            r.sts = sts; r.src = src;
            if (head - tail >= QSIZE) {
                tail = __atomic_load_n(&q_tail, __ATOMIC_ACQUIRE);
                if (head - tail >= QSIZE) { __atomic_add_fetch(&st_drop, 1, __ATOMIC_RELAXED); continue; }
            }
            queue[head & QMASK] = r;
            head++;
            got = 1;
        }
        __atomic_store_n(&q_head, head, __ATOMIC_RELEASE);
        if (head - tail > __atomic_load_n(&st_qmax, __ATOMIC_RELAXED))
            __atomic_store_n(&st_qmax, (uint64_t)(head - tail), __ATOMIC_RELAXED);
        __atomic_store_n(&st_missed, ring.missed, __ATOMIC_RELAXED);

        uint64_t done = now_ns(CLOCK_MONOTONIC);
        if (got) {
            int64_t off = (int64_t)now_ns(CLOCK_REALTIME) - (int64_t)(ring.last * NS_PER_TICK);
            if (off < win_min) win_min = off;
            if (off > win_max) win_max = off;
        }
        if (done - woke > __atomic_load_n(&st_loop_max_ns, __ATOMIC_RELAXED))
            __atomic_store_n(&st_loop_max_ns, done - woke, __ATOMIC_RELAXED);
        __atomic_add_fetch(&st_polls, 1, __ATOMIC_RELAXED);

        if (done >= win_end) {
            if (win_min != INT64_MAX) {
                if (!have_best || win_min < best || win_min - best > 1000000) { best = win_min; have_best = 1; }
                __atomic_store_n(&st_offset, best, __ATOMIC_RELAXED);
                __atomic_store_n(&st_win_spread, win_max - win_min, __ATOMIC_RELAXED);
                __atomic_store_n(&st_offset_valid, 1, __ATOMIC_RELEASE);
            }
            win_min = INT64_MAX; win_max = INT64_MIN;
            win_end = done + 1000000000ull;
        }
    }
    return NULL;
}

/* -------------------------------------------------------- HTTP output */

static char out_path[480];
static char *out; static size_t out_len, out_cap;
static uint64_t st_http_ok, st_http_fail, st_lines;
static long out_lines;

static int out_reserve(size_t more)
{
    if (out_len + more + 1 <= out_cap) return 0;
    size_t nc = out_cap ? out_cap : (1 << 20);
    while (nc < out_len + more + 1) nc *= 2;
    char *n = realloc(out, nc);
    if (!n) return -1;
    out = n; out_cap = nc;
    return 0;
}

#define EMIT(...) do { \
    if (out_reserve(1024) == 0) out_len += (size_t)snprintf(out + out_len, out_cap - out_len, __VA_ARGS__); \
} while (0)

static int http_post(const char *body, size_t len, char *msg, size_t msglen)
{
    struct addrinfo hints = { 0 }, *ai = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int e = getaddrinfo(o.host, o.port, &hints, &ai);
    if (e) { snprintf(msg, msglen, "%s", gai_strerror(e)); return -1; }
    int fd = -1;
    for (struct addrinfo *a = ai; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv = { 2, 0 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(ai);
    if (fd < 0) { snprintf(msg, msglen, "connect %s:%s: %s", o.host, o.port, strerror(errno)); return -1; }

    char hdr[1024];
    int hl = snprintf(hdr, sizeof hdr,
        "POST %s HTTP/1.1\r\nHost: %s:%s\r\nAuthorization: Token %s\r\n"
        "Content-Type: text/plain; charset=utf-8\r\nAccept: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n",
        out_path, o.host, o.port, o.token, len);
    const char *parts[2] = { hdr, body };
    size_t lens[2] = { (size_t)hl, len };
    for (int p = 0; p < 2; p++) {
        size_t off = 0;
        while (off < lens[p]) {
            ssize_t w = send(fd, parts[p] + off, lens[p] - off, MSG_NOSIGNAL);
            if (w < 0) {
                if (errno == EINTR) continue;
                snprintf(msg, msglen, "send: %s", strerror(errno));
                close(fd); return -1;
            }
            off += (size_t)w;
        }
    }
    char resp[2048];
    size_t rn = 0;
    for (;;) {
        ssize_t g = recv(fd, resp + rn, sizeof resp - 1 - rn, 0);
        if (g < 0 && errno == EINTR) continue;
        if (g <= 0) break;
        rn += (size_t)g;
        if (rn >= sizeof resp - 1) break;
    }
    close(fd);
    resp[rn] = 0;
    int status = -1;
    if (sscanf(resp, "HTTP/%*s %d", &status) != 1) {
        snprintf(msg, msglen, "no HTTP status in response (%zu bytes)", rn);
        return -1;
    }
    msg[0] = 0;
    if (status / 100 != 2) {
        char *b = strstr(resp, "\r\n\r\n");
        b = b ? b + 4 : resp;
        snprintf(msg, msglen, "%.*s", (int)strcspn(b, "\r\n"), b);
    }
    return status;
}

static uint64_t last_err_ns;

static void flush_out(void)
{
    if (!out_len) return;
    char msg[512];
    int st = http_post(out, out_len, msg, sizeof msg);
    if (st < 200 || st >= 300) {
        if (st < 0 || st >= 500) { nap_us(100000); st = http_post(out, out_len, msg, sizeof msg); }
    }
    if (st >= 200 && st < 300) {
        st_http_ok++;
        st_lines += (uint64_t)out_lines;
    } else {
        st_http_fail++;
        uint64_t t = now_ns(CLOCK_MONOTONIC);
        if (t - last_err_ns > 5000000000ull) {
            fprintf(stderr, "adcscope: write failed (%s%d %s); dropping %ld lines\n",
                    st < 0 ? "error " : "HTTP ", st, msg, out_lines);
            last_err_ns = t;
        }
    }
    out_len = 0; out_lines = 0;
}

/* ------------------------------------------------------------- worker */

static char *T_node, *T_sess;           /* escaped tag values */
static int64_t offset;

static int64_t tns(uint64_t pl) { return (int64_t)(pl * NS_PER_TICK) + offset; }

static void names(char *dst, size_t n, uint32_t bits)
{
    size_t j = 0;
    dst[0] = 0;
    for (int b = 0; b < NTRIPBIT; b++)
        if (bits & (1u << b)) j += (size_t)snprintf(dst + j, n - j, "%s%s", j ? "," : "", trip_bit[b]);
    if (bits >> NTRIPBIT) j += (size_t)snprintf(dst + j, n - j, "%sother_0x%X", j ? "," : "", bits >> NTRIPBIT);
    if (!j) snprintf(dst, n, "none");
}

/* --- cycle windows */

struct win {
    size_t n;
    uint64_t ts[WMAX];
    int16_t x[NCH][WMAX];
};
static struct win W;
static double ref_dc, ref_pp;          /* from the previous window */
static int armed, have_cross;
static double last_cross;              /* interpolated crossing, ticks since base */
static uint64_t tick_base;
static int16_t prev_ref;
static uint64_t prev_ts;
static uint32_t cur_sts, cur_src;
static uint64_t n_cycles;

static void emit_cycle(int valid, double period_ticks)
{
    size_t n = W.n;
    if (n < 2) return;
    double mean[NCH], rms[NCH];
    EMIT("cycle,node_id=%s,session=%s ", T_node, T_sess);
    int first = 1;
    for (int c = 0; c < NCH; c++) {
        double s = 0, sq = 0; int mn = 32767, mx = -32768;
        for (size_t i = 0; i < n; i++) {
            int v = W.x[c][i];
            s += v; sq += (double)v * v;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        mean[c] = s / n;
        double var = sq / n - mean[c] * mean[c];
        rms[c] = var > 0 ? sqrt(var) : 0;
        if (c == o.ref) { ref_dc = mean[c]; ref_pp = mx - mn; }
        if (!(o.chmask & (1u << c))) continue;
        EMIT("%sch%d_mean=%.6g,ch%d_rms=%.6g,ch%d_pp=%.6g", first ? "" : ",",
             c, scl(c, mean[c]), c, scl(c, rms[c]), c, scl(c, mx - mn));
        first = 0;
    }
    if (valid) {
        EMIT(",freq=%.5f", TICK_HZ / period_ticks);
        size_t k = (size_t)llround(n / 4.0);
        for (int p = 0; p < o.npair; p++) {
            int v = o.pv[p], i = o.pi[p];
            double sp = 0, sq = 0;
            for (size_t j = 0; j < n; j++) {
                double vi = W.x[v][j] - mean[v];
                double vd = W.x[v][(j + n - k) % n] - mean[v];   /* v delayed a quarter cycle */
                double ii = W.x[i][j] - mean[i];
                sp += vi * ii; sq += vd * ii;
            }
            double P = scl(v, scl(i, sp / n)), Q = scl(v, scl(i, sq / n));
            double S = scl(v, rms[v]) * scl(i, rms[i]);
            EMIT(",p%d_%d=%.6g,q%d_%d=%.6g", v, i, P, v, i, Q);
            if (S > 0) EMIT(",pf%d_%d=%.5f", v, i, P / S);
        }
    }
    EMIT(",n=%zui", n);
    if (!o.no_trip) EMIT(",sts=%ui,src=%ui", cur_sts, cur_src);
    EMIT(" %" PRId64 "\n", tns(W.ts[0]));
    out_lines++;
    n_cycles++;
    W.n = 0;
}

static void cycle_sample(const struct rec *r)
{
    int16_t xr = r->code[o.ref];
    int crossed = 0;
    double t_cross = 0;
    if (ref_pp >= MINPP && W.n > 0) {
        double hyst = ref_pp * 0.1;
        double v0 = prev_ref - ref_dc, v1 = xr - ref_dc;
        if (v1 < -hyst) armed = 1;
        if (armed && v0 < 0 && v1 >= 0) {
            double frac = v0 / (v0 - v1);
            t_cross = (double)(prev_ts - tick_base) + frac * (double)(r->ts - prev_ts);
            crossed = 1;
            armed = 0;
        }
    }
    if (crossed) {
        if (have_cross) emit_cycle(1, t_cross - last_cross);
        else W.n = 0;                      /* first crossing: start aligned */
        last_cross = t_cross;
        have_cross = 1;
    } else if ((ref_pp < MINPP && W.n >= WFIX) || W.n >= WAC_MAX) {
        emit_cycle(0, 0);
        have_cross = 0;
    }
    if (W.n < WMAX) {
        W.ts[W.n] = r->ts;
        for (int c = 0; c < NCH; c++) W.x[c][W.n] = r->code[c];
        W.n++;
    }
    prev_ref = xr;
    prev_ts = r->ts;
}

/* --- decimated waveform */

static long dec, dec_n;
static double dec_sum[NCH];
static uint64_t dec_ts0;

static void wave_sample(const struct rec *r)
{
    if (dec <= 0) return;
    if (!dec_n) dec_ts0 = r->ts;
    for (int c = 0; c < NCH; c++) dec_sum[c] += r->code[c];
    if (++dec_n < dec) return;
    EMIT("wave,node_id=%s,session=%s ", T_node, T_sess);
    int first = 1;
    for (int c = 0; c < NCH; c++) {
        if (!(o.chmask & (1u << c))) continue;
        EMIT("%sch%d=%.6g", first ? "" : ",", c, scl(c, dec_sum[c] / dec));
        first = 0;
    }
    EMIT(" %" PRId64 "\n", tns(dec_ts0));
    out_lines++;
    memset(dec_sum, 0, sizeof dec_sum);
    dec_n = 0;
}

/* --- captures */

static struct rec *hist; static size_t hist_cap, hist_n, hist_pos;
static struct rec *cap; static size_t cap_n, cap_need, cap_max;
static const char *cap_tag;
static int capturing;
static uint64_t n_captures, n_trig_ignored;

static void start_capture(const char *tag)
{
    if (capturing) { n_trig_ignored++; return; }
    size_t k = hist_n < hist_cap ? hist_n : hist_cap;
    if (k) {
        size_t start = (hist_pos + hist_cap - k) % hist_cap;
        for (size_t i = 0; i < k; i++) cap[i] = hist[(start + i) % hist_cap];
    }
    cap_n = k;
    cap_need = k + (size_t)(o.post_s * REC_HZ);
    if (cap_need > cap_max) cap_need = cap_max;
    cap_tag = tag;
    capturing = 1;
    fprintf(stderr, "adcscope: %s trigger, capturing %.3f s before and %.3f s after\n",
            tag, k / REC_HZ, o.post_s);
}

static void finish_capture(void)
{
    flush_out();                           /* keep the live stream ahead of the burst */
    uint64_t miss = 0;
    for (size_t i = 0; i < cap_n; i++) {
        const struct rec *s = &cap[i];
        if (i) {
            long long k = llround((double)(s->ts - cap[i - 1].ts) / TICKS_PER_REC);
            if (k > 1) miss += (uint64_t)(k - 1);
        }
        EMIT("burst,node_id=%s,session=%s,tag=%s ", T_node, T_sess, cap_tag);
        int first = 1;
        for (int c = 0; c < NCH; c++) {
            if (!(o.chmask & (1u << c))) continue;
            EMIT("%sch%d=%di", first ? "" : ",", c, s->code[c]);
            if (o.gain[c] > 0) EMIT(",ch%d_v=%.5f", c, s->code[c] / o.gain[c]);
            first = 0;
        }
        EMIT(",pl=%" PRIu64 "i,sts=%ui %" PRId64 "\n", s->ts, s->sts, tns(s->ts));
        out_lines++;
        if (out_len > (4u << 20)) flush_out();
    }
    double span = cap_n > 1 ? (double)(cap[cap_n - 1].ts - cap[0].ts) / TICK_HZ : 0;
    EMIT("burst_meta,node_id=%s,session=%s,tag=%s samples=%zui,missed=%" PRIu64 "i,"
         "duration_s=%.6f,rate_hz=%.4f,chmask=%ui,first_pl=%" PRIu64 "i,offset_ns=%" PRId64 "i %" PRId64 "\n",
         T_node, T_sess, cap_tag, cap_n, miss, span, span > 0 ? (cap_n - 1) / span : 0,
         o.chmask, cap[0].ts, offset, tns(cap[0].ts));
    out_lines++;
    flush_out();
    fprintf(stderr, "adcscope: %s capture written, %zu samples, %" PRIu64 " missed\n", cap_tag, cap_n, miss);
    capturing = 0;
    n_captures++;
}

static void capture_sample(const struct rec *r)
{
    if (hist_cap) { hist[hist_pos] = *r; hist_pos = (hist_pos + 1) % hist_cap; if (hist_n < hist_cap) hist_n++; }
    if (capturing) {
        if (cap_n < cap_max) cap[cap_n++] = *r;
        if (cap_n >= cap_need) finish_capture();
    }
}

/* --- trip status */

static int have_trip;

static void trip_sample(const struct rec *r)
{
    if (o.no_trip) return;
    if (have_trip && r->sts == cur_sts && r->src == cur_src) return;
    char a[160], s[160];
    names(a, sizeof a, r->sts);
    names(s, sizeof s, r->src);
    EMIT("trip,node_id=%s,session=%s sts=%ui,src=%ui,prev_sts=%ui,active=\"%s\",cause=\"%s\" %" PRId64 "\n",
         T_node, T_sess, r->sts, r->src, have_trip ? cur_sts : r->sts, a, s, tns(r->ts));
    out_lines++;
    fprintf(stderr, "adcscope: trip status 0x%X (%s), source 0x%X (%s)\n", r->sts, a, r->src, s);
    int rising = have_trip && cur_sts == 0 && r->sts != 0;
    cur_sts = r->sts; cur_src = r->src;
    have_trip = 1;
    if (rising) start_capture("trip");
}

static void emit_meta(uint64_t now_rt)
{
    uint64_t drop = __atomic_load_n(&st_drop, __ATOMIC_RELAXED);
    uint64_t miss = __atomic_load_n(&st_missed, __ATOMIC_RELAXED);
    uint64_t qmax = __atomic_exchange_n(&st_qmax, 0, __ATOMIC_RELAXED);
    uint64_t wake = __atomic_exchange_n(&st_wake_max_ns, 0, __ATOMIC_RELAXED);
    uint64_t loop = __atomic_exchange_n(&st_loop_max_ns, 0, __ATOMIC_RELAXED);
    int64_t spread = __atomic_load_n(&st_win_spread, __ATOMIC_RELAXED);
    EMIT("scope_meta,node_id=%s,session=%s q_max=%" PRIu64 "i,q_drop=%" PRIu64 "i,missed=%" PRIu64 "i,"
         "http_ok=%" PRIu64 "i,http_fail=%" PRIu64 "i,lines=%" PRIu64 "i,cycles=%" PRIu64 "i,"
         "captures=%" PRIu64 "i,wake_max_us=%.1f,drain_max_us=%.1f,offset_ns=%" PRId64 "i,"
         "offset_spread_ns=%" PRId64 "i %" PRIu64 "\n",
         T_node, T_sess, qmax, drop, miss, st_http_ok, st_http_fail, st_lines, n_cycles,
         n_captures, wake / 1e3, loop / 1e3, offset, spread, now_rt);
    out_lines++;
    fprintf(stderr, "adcscope: queue max %" PRIu64 ", dropped %" PRIu64 ", missed %" PRIu64
            ", posts ok %" PRIu64 " failed %" PRIu64 ", wake late max %.0f us, drain max %.0f us\n",
            qmax, drop, miss, st_http_ok, st_http_fail, wake / 1e3, loop / 1e3);
}

static void worker(void)
{
    uint64_t t_start = now_ns(CLOCK_MONOTONIC);
    uint64_t t_end = o.duration > 0 ? t_start + (uint64_t)(o.duration * 1e9) : 0;
    uint64_t next_flush = t_start + (uint64_t)o.flush_ms * 1000000ull;
    uint64_t next_meta = t_start + 10000000000ull;
    int started = 0;

    while (!stop && (!t_end || now_ns(CLOCK_MONOTONIC) < t_end)) {
        if (!__atomic_load_n(&st_offset_valid, __ATOMIC_ACQUIRE)) { nap_us(10000); continue; }
        offset = __atomic_load_n(&st_offset, __ATOMIC_RELAXED);
        if (!started) {
            fprintf(stderr, "adcscope: streaming to %s:%s bucket %s, session %s\n",
                    o.host, o.port, o.bucket, o.session);
            started = 1;
        }
        if (trig_manual) { trig_manual = 0; start_capture("manual"); }

        uint32_t tail = q_tail;
        uint32_t head = __atomic_load_n(&q_head, __ATOMIC_ACQUIRE);
        uint32_t lim = 4000;               /* yield to flushing at least every 200 ms of data */
        while (tail != head && lim--) {
            const struct rec *r = &queue[tail & QMASK];
            if (!tick_base) tick_base = r->ts;
            trip_sample(r);
            cycle_sample(r);
            wave_sample(r);
            capture_sample(r);
            tail++;
        }
        __atomic_store_n(&q_tail, tail, __ATOMIC_RELEASE);

        uint64_t t = now_ns(CLOCK_MONOTONIC);
        if (t >= next_meta) { emit_meta(now_ns(CLOCK_REALTIME)); next_meta = t + 10000000000ull; }
        if (t >= next_flush || out_len > (4u << 20)) { flush_out(); next_flush = t + (uint64_t)o.flush_ms * 1000000ull; }
        if (tail == head) nap_us(2000);
    }
    emit_meta(now_ns(CLOCK_REALTIME));
    flush_out();
}

/* --------------------------------------------------------------- main */

static void usage(void)
{
    fprintf(stderr,
        "usage: adcscope [options]\n"
        "  -c LIST    channels to publish (default 0,1,2,3,4,5)\n"
        "  -R CH      reference channel for cycle detection (default: first in -c)\n"
        "  -W V:I,..  voltage:current channel pairs for P, Q and PF (default none)\n"
        "  -r HZ      waveform rate, block-averaged; 0 = off, 20000 = every sample (default 1000)\n"
        "  -T PRE:POST  capture seconds before:after a trigger (default 0.5:0.5, total <= 10)\n"
        "  -N         do not read the trip registers (no trip points or trip captures)\n"
        "  -t POLLS   read trip registers every POLLS drain polls (default 10)\n"
        "  -H HOST    InfluxDB host (default 192.168.50.1)\n"
        "  -P PORT    InfluxDB port (default 8086)\n"
        "  -O ORG     org (default qut-microgrid)\n"
        "  -b BUCKET  bucket (default telemetry)\n"
        "  -k FILE    token file (default /mnt/sd/influx.token, or $INFLUX_TOKEN)\n"
        "  -n NODE    node_id tag (default $NODE_ID or cosmos-test)\n"
        "  -s ID      session tag (default scope-<unix time>; try -s $(philics-runinc))\n"
        "  -f MS      write interval (default 500)\n"
        "  -d SEC     stop after SEC seconds (default 0 = run until Ctrl+C)\n"
        "  -g CONF    gains file (default /mnt/sd/adcmon.conf)\n"
        "  -m DEV     memory device (default /dev/mem)\n"
        "  -p US      drain poll period (default 1000)\n"
        "  -q PRIO    drain thread SCHED_FIFO priority (default 50)\n"
        "Manual capture: killall -USR1 adcscope\n"
        "Run adcmon start (or labup) first.\n");
}

int main(int argc, char **argv)
{
    const char *envnode = getenv("NODE_ID");
    o = (struct opts){
        .dev = "/dev/mem", .conf = "/mnt/sd/adcmon.conf", .host = "192.168.50.1", .port = "8086",
        .org = "qut-microgrid", .bucket = "telemetry", .tokfile = "/mnt/sd/influx.token",
        .node = envnode && *envnode ? envnode : "cosmos-test", .chmask = 0x3F, .ref = -1,
        .wave_hz = 1000, .pre_s = 0.5, .post_s = 0.5, .poll_us = 1000, .flush_ms = 500,
        .trip_every = 10, .prio = 50,
    };
    char sessbuf[64];
    int opt;
    while ((opt = getopt(argc, argv, "c:R:W:r:T:Nt:H:P:O:b:k:n:s:f:d:g:m:p:q:h")) != -1) {
        switch (opt) {
        case 'c': o.chmask = parse_mask(optarg); break;
        case 'R': o.ref = atoi(optarg); break;
        case 'W': if (parse_pairs(optarg)) { fprintf(stderr, "adcscope: bad -W %s\n", optarg); return 2; } break;
        case 'r': o.wave_hz = atof(optarg); break;
        case 'T': if (sscanf(optarg, "%lf:%lf", &o.pre_s, &o.post_s) != 2) { usage(); return 2; } break;
        case 'N': o.no_trip = 1; break;
        case 't': o.trip_every = atol(optarg); break;
        case 'H': o.host = optarg; break;
        case 'P': o.port = optarg; break;
        case 'O': o.org = optarg; break;
        case 'b': o.bucket = optarg; break;
        case 'k': o.tokfile = optarg; break;
        case 'n': o.node = optarg; break;
        case 's': o.session = optarg; break;
        case 'f': o.flush_ms = atol(optarg); break;
        case 'd': o.duration = atof(optarg); break;
        case 'g': o.conf = optarg; break;
        case 'm': o.dev = optarg; break;
        case 'p': o.poll_us = atol(optarg); break;
        case 'q': o.prio = atoi(optarg); break;
        case 'h': usage(); return 0;
        default: usage(); return 2;
        }
    }
    if (optind < argc || !o.chmask || o.wave_hz < 0 || o.wave_hz > 20000 || o.pre_s < 0 || o.post_s < 0 ||
        o.pre_s + o.post_s > 10 || o.trip_every < 1 || o.flush_ms < 50 || o.poll_us < 100 ||
        o.poll_us > 20000 || o.prio < 1 || o.prio > 99) {
        usage(); return 2;
    }
    if (o.ref < 0) for (int c = 0; c < NCH; c++) if (o.chmask & (1u << c)) { o.ref = c; break; }
    if (o.ref < 0 || o.ref >= NCH) { fprintf(stderr, "adcscope: bad -R\n"); return 2; }
    dec = o.wave_hz > 0 ? lround(REC_HZ / o.wave_hz) : 0;
    if (o.wave_hz > 0 && dec < 1) dec = 1;
    if (dec == 1) fprintf(stderr, "adcscope: note: full-rate waveform; check queue drops in scope_meta\n");

    if (!o.session) {
        snprintf(sessbuf, sizeof sessbuf, "scope-%" PRIu64, (uint64_t)(now_ns(CLOCK_REALTIME) / 1000000000ull));
        o.session = sessbuf;
    }
    if (now_ns(CLOCK_REALTIME) < 1700000000ull * 1000000000ull) {
        fprintf(stderr, "adcscope: system clock not set; set it before streaming\n");
        return 1;
    }
    o.token = read_token();
    if (!o.token) { fprintf(stderr, "adcscope: no token (put it in %s or set INFLUX_TOKEN)\n", o.tokfile); return 1; }
    load_gains();
    T_node = tagval(o.node); T_sess = tagval(o.session);
    {
        char eo[200], eb[200];
        urlenc(eo, sizeof eo, o.org);
        urlenc(eb, sizeof eb, o.bucket);
        snprintf(out_path, sizeof out_path, "/api/v2/write?org=%s&bucket=%s&precision=ns", eo, eb);
    }

    queue = calloc(QSIZE, sizeof *queue);
    hist_cap = (size_t)(o.pre_s * REC_HZ);
    cap_max = (size_t)((o.pre_s + o.post_s) * REC_HZ) + 64;
    hist = hist_cap ? calloc(hist_cap, sizeof *hist) : NULL;
    cap = calloc(cap_max, sizeof *cap);
    if (!queue || (hist_cap && !hist) || !cap || out_reserve(1 << 20)) { perror("calloc"); return 1; }
    if (mlockall(MCL_CURRENT | MCL_FUTURE))
        fprintf(stderr, "adcscope: mlockall failed (%s); continuing\n", strerror(errno));

    struct sigaction sa = { 0 };
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    ring.p = map_phys(o.dev, RING_BASE, NREC * STRIDE);
    if (!ring.p) return 1;
    if (!o.no_trip) {
        plregs = map_phys(o.dev, PL_BASE, 0x1000);
        if (!plregs) return 1;
    }
    if (ring_attach(&ring) < 0) {
        fprintf(stderr, "adcscope: no consistent write head; is the DAQ running? (run: adcmon start)\n");
        return 1;
    }

    fprintf(stderr, "adcscope: channels 0x%X, reference ch%d, %d power pair(s), waveform %s, "
                    "trip registers %s, capture %.2f:%.2f s\n",
            o.chmask, o.ref, o.npair, dec ? "on" : "off", o.no_trip ? "not read" : "read",
            o.pre_s, o.post_s);
    for (int c = 0; c < NCH; c++)
        if (o.chmask & (1u << c))
            fprintf(stderr, "adcscope:   ch%d %-5s %s\n", c, ch_name[c],
                    o.gain[c] > 0 ? "scaled by adcmon.conf" : "ADC codes (no gain set)");
    if (dec) fprintf(stderr, "adcscope: waveform every %ld samples (%.1f Hz)\n", dec, REC_HZ / dec);

    pthread_t th;
    if (pthread_create(&th, NULL, drain_main, NULL)) { perror("pthread_create"); return 1; }
    worker();
    stop = 1;
    pthread_join(th, NULL);
    return 0;
}
