/*
 * adcburst - full-rate (20 kHz) burst capture from the PHiLICS OCM ring,
 * written to InfluxDB v2 as line protocol over HTTP.
 *
 *   adcburst [-d SEC] [-c 0,1] [-H HOST] [-P PORT] [-O ORG] [-b BUCKET]
 *            [-k TOKENFILE] [-n NODE] [-s SESSION] [-t TAG] [-M MEAS]
 *            [-B LINES] [-w FILE]
 *
 * The run has two phases.
 *   1. Capture. Every record for -d seconds is copied into RAM. No network
 *      or file I/O happens in this phase, so the OCM read pattern matches
 *      adcmon record.
 *   2. Upload. The samples are formatted as line protocol and POSTed to
 *      /api/v2/write?precision=ns in batches of -B lines (default 5000).
 *      With -w FILE the lines go to FILE instead (use /tmp), which can be
 *      written later with `influx write` from the laptop.
 *
 * Each sample becomes one point in measurement -M (default "burst"):
 *   burst,node_id=N,session=S,tag=T ch0=123i,ch0_v=0.62278,...,pl=987i <ns>
 * chN is the ADC code (raw >> 4) as an integer field, chN_v is volts when
 * the gains file has a gain for that channel, pl is the 64-bit PL counter.
 * Timestamps are integer nanoseconds, so this path does not go through
 * Telegraf's float64 JSON parser (Risk 8).
 *
 * One summary point goes in "<MEAS>_meta" with sample count, missed
 * records, measured rate and the timestamp anchor.
 *
 * Timestamp model: t_ns = pl * 5 + offset. After each poll drains the ring,
 * the last record consumed is the newest one written, so
 * (CLOCK_REALTIME after the drain) - pl*5 is the true offset plus the age
 * of that record (0 to ~50 us plus read time). The smallest value over all
 * polls is used, and the spread (largest - smallest) is reported as
 * offset_spread_ns. The 5 ns/tick slope is nominal; the measured PL vs CPU
 * drift (-0.020 ppm) is 0.1 ns over a 5 s burst.
 *
 * The token is read from -k FILE (default /mnt/sd/influx.token) or from
 * the INFLUX_TOKEN environment variable.
 *
 * Exit codes: 0 ok, 1 setup or upload failure, 2 usage, 3 records missed
 * during capture (data is still uploaded).
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <netdb.h>
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
#define NCH           6
#define TICK_HZ       200000000.0
#define NS_PER_TICK   5
#define TICKS_PER_REC 10000.017   /* measured 19,999.966 Hz */
#define REC_HZ        (TICK_HZ / TICKS_PER_REC)
#define MAX_DUR       60.0

static volatile sig_atomic_t stop;
static void on_signal(int s) { (void)s; stop = 1; }

struct opts {
    const char *dev, *conf, *host, *port, *org, *bucket, *tokfile;
    const char *node, *session, *tag, *meas, *wfile;
    char *token;
    unsigned chmask;
    double duration;
    long poll_us, batch;
    double gain[NCH];
};

struct rec { uint64_t ts; int16_t code[NCH]; };

struct ring {
    volatile uint32_t *p;
    unsigned idx;
    uint64_t last;
    uint64_t overruns, missed;
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

/* Tag values: spaces, commas, equals signs and quotes become '_'. */
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

static char *read_token(const struct opts *o)
{
    const char *env = getenv("INFLUX_TOKEN");
    if (env && *env) return strdup(env);
    FILE *f = fopen(o->tokfile, "r");
    if (!f) return NULL;
    char buf[512] = { 0 };
    if (!fgets(buf, sizeof buf, f)) buf[0] = 0;
    fclose(f);
    buf[strcspn(buf, "\r\n \t")] = 0;
    return buf[0] ? strdup(buf) : NULL;
}

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

static int ring_next(struct ring *r, struct rec *out)
{
    for (;;) {
        uint64_t ts = read_ts(r, r->idx);
        if (ts <= r->last) return 0;
        volatile uint32_t *q = r->p + r->idx * (STRIDE / 4);
        uint32_t w[3] = { q[2], q[3], q[4] };
        if (read_ts(r, r->idx) != ts) continue;
        long long k = llround((double)(ts - r->last) / TICKS_PER_REC);
        if (k > 1) { r->overruns++; r->missed += (uint64_t)(k - 1); }
        out->ts = ts;
        for (int c = 0; c < NCH; c++)
            out->code[c] = (int16_t)((int16_t)(uint16_t)(w[c / 2] >> ((c & 1) ? 16 : 0)) >> 4);
        r->last = ts;
        r->idx = (r->idx + 1) % NREC;
        return 1;
    }
}

/* --------------------------------------------------------------- capture */

struct capture {
    struct rec *s;
    size_t n, cap;
    uint64_t missed, overruns;
    int64_t off_min, off_max;     /* realtime_ns - pl*5 */
    long polls;
};

static int capture(struct opts *o, struct ring *r, struct capture *cp)
{
    cp->cap = (size_t)(o->duration * REC_HZ * 1.02) + 4096;
    cp->s = malloc(cp->cap * sizeof *cp->s);
    if (!cp->s) { perror("malloc"); return 1; }
    memset(cp->s, 0, cp->cap * sizeof *cp->s);   /* touch pages before capture */
    cp->off_min = INT64_MAX; cp->off_max = INT64_MIN;

    uint64_t miss0 = r->missed, ovr0 = r->overruns;
    uint64_t t_end = now_ns(CLOCK_MONOTONIC) + (uint64_t)(o->duration * 1e9);
    while (!stop && now_ns(CLOCK_MONOTONIC) < t_end && cp->n < cp->cap) {
        size_t before = cp->n;
        while (cp->n < cp->cap && ring_next(r, &cp->s[cp->n]) == 1) cp->n++;
        if (cp->n > before) {
            int64_t off = (int64_t)now_ns(CLOCK_REALTIME) - (int64_t)(r->last * NS_PER_TICK);
            if (off < cp->off_min) cp->off_min = off;
            if (off > cp->off_max) cp->off_max = off;
            cp->polls++;
        }
        nap(o->poll_us);
    }
    cp->missed = r->missed - miss0;
    cp->overruns = r->overruns - ovr0;
    return cp->n ? 0 : 1;
}

/* ------------------------------------------------------------------ http */

/* POST body; returns the HTTP status, or -1 on a connection error.
 * The first line of any non-2xx body is copied to msg. */
static int http_post(const struct opts *o, const char *path, const char *body, size_t len,
                     char *msg, size_t msglen)
{
    struct addrinfo hints = { 0 }, *ai = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int e = getaddrinfo(o->host, o->port, &hints, &ai);
    if (e) { snprintf(msg, msglen, "%s", gai_strerror(e)); return -1; }

    int fd = -1;
    for (struct addrinfo *a = ai; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv = { 10, 0 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(ai);
    if (fd < 0) { snprintf(msg, msglen, "connect to %s:%s: %s", o->host, o->port, strerror(errno)); return -1; }

    char hdr[1024];
    int hl = snprintf(hdr, sizeof hdr,
        "POST %s HTTP/1.1\r\nHost: %s:%s\r\nAuthorization: Token %s\r\n"
        "Content-Type: text/plain; charset=utf-8\r\nAccept: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n",
        path, o->host, o->port, o->token, len);
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

    char resp[4096];
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

/* ---------------------------------------------------------------- upload */

struct sink {
    const struct opts *o;
    FILE *wf;
    char path[480];
    long sent_batches, sent_lines;
};

static int sink_flush(struct sink *k, const char *buf, size_t len, long lines)
{
    if (!len) return 0;
    if (k->wf) {
        if (fwrite(buf, 1, len, k->wf) != len) { perror(k->o->wfile); return -1; }
        k->sent_lines += lines; k->sent_batches++;
        return 0;
    }
    char msg[512];
    int st = -1;
    for (int attempt = 0; attempt < 3 && !stop; attempt++) {
        st = http_post(k->o, k->path, buf, len, msg, sizeof msg);
        if (st >= 200 && st < 300) break;
        fprintf(stderr, "adcburst: batch %ld: %s%d %s\n", k->sent_batches + 1,
                st < 0 ? "error " : "HTTP ", st, msg);
        if (st == 400 || st == 401 || st == 403 || st == 404 || st == 413) return -1; /* not transient */
        nap(500000 * (attempt + 1));
    }
    if (st < 200 || st >= 300) return -1;
    k->sent_lines += lines; k->sent_batches++;
    return 0;
}

static int upload(struct opts *o, const struct capture *cp, int64_t off)
{
    struct sink k = { .o = o };
    if (o->wfile) {
        k.wf = fopen(o->wfile, "w");
        if (!k.wf) { perror(o->wfile); return 1; }
    } else {
        char eo[200], eb[200];
        urlenc(eo, sizeof eo, o->org);
        urlenc(eb, sizeof eb, o->bucket);
        snprintf(k.path, sizeof k.path, "/api/v2/write?org=%s&bucket=%s&precision=ns", eo, eb);
    }

    char *node = tagval(o->node), *sess = tagval(o->session), *tag = tagval(o->tag);
    char *meas = tagval(o->meas);
    char prefix[512];
    int pl = snprintf(prefix, sizeof prefix, "%s,node_id=%s,session=%s,tag=%s ", meas, node, sess, tag);

    size_t line_max = (size_t)pl + 40 * NCH + 64;
    size_t bufcap = line_max * (size_t)o->batch + 1024;
    char *buf = malloc(bufcap);
    if (!buf) { perror("malloc"); return 1; }

    uint64_t t0 = now_ns(CLOCK_MONOTONIC);
    size_t len = 0; long lines = 0;
    int rc = 0;
    for (size_t i = 0; i < cp->n && !stop; i++) {
        const struct rec *s = &cp->s[i];
        memcpy(buf + len, prefix, (size_t)pl); len += (size_t)pl;
        int first = 1;
        for (int c = 0; c < NCH; c++) {
            if (!(o->chmask & (1u << c))) continue;
            len += (size_t)sprintf(buf + len, "%sch%d=%di", first ? "" : ",", c, s->code[c]);
            first = 0;
            if (o->gain[c] > 0)
                len += (size_t)sprintf(buf + len, ",ch%d_v=%.5f", c, s->code[c] / o->gain[c]);
        }
        int64_t t = (int64_t)(s->ts * NS_PER_TICK) + off;
        len += (size_t)sprintf(buf + len, ",pl=%" PRIu64 "i %" PRId64 "\n", s->ts, t);
        if (++lines >= o->batch) {
            if (sink_flush(&k, buf, len, lines) < 0) { rc = 1; break; }
            len = 0; lines = 0;
        }
    }
    if (!rc && !stop && lines && sink_flush(&k, buf, len, lines) < 0) rc = 1;

    /* Summary point, timestamped at the first sample. */
    if (!rc && !stop && cp->n) {
        double span = cp->n > 1 ? (double)(cp->s[cp->n - 1].ts - cp->s[0].ts) / TICK_HZ : 0;
        len = (size_t)sprintf(buf,
            "%s_meta,node_id=%s,session=%s,tag=%s samples=%zui,missed=%" PRIu64 "i,"
            "duration_s=%.6f,rate_hz=%.4f,chmask=%ui,first_pl=%" PRIu64 "i,"
            "offset_ns=%" PRId64 "i,offset_spread_ns=%" PRId64 "i %" PRId64 "\n",
            meas, node, sess, tag, cp->n, cp->missed, span,
            span > 0 ? (double)(cp->n - 1) / span : 0, o->chmask, cp->s[0].ts,
            off, cp->off_max - cp->off_min,
            (int64_t)(cp->s[0].ts * NS_PER_TICK) + off);
        if (sink_flush(&k, buf, len, 1) < 0) rc = 1;
    }

    double el = (double)(now_ns(CLOCK_MONOTONIC) - t0) / 1e9;
    fprintf(stderr, "adcburst: %s %ld lines in %ld batches in %.2f s -> %s\n",
            rc ? "FAILED after" : "wrote", k.sent_lines, k.sent_batches, el,
            o->wfile ? o->wfile : o->host);
    if (k.wf && fclose(k.wf)) { perror(o->wfile); rc = 1; }
    free(buf); free(node); free(sess); free(tag); free(meas);
    return rc;
}

/* ------------------------------------------------------------------ main */

static void usage(void)
{
    fprintf(stderr,
        "usage: adcburst [-d SEC] [-c 0,1] [-H HOST] [-P PORT] [-O ORG] [-b BUCKET]\n"
        "                [-k TOKENFILE] [-n NODE] [-s SESSION] [-t TAG] [-M MEAS]\n"
        "                [-B LINES] [-w FILE]\n"
        "  -d SEC     capture length, 0-%g s (default 1)\n"
        "  -c LIST    channels (default 0,1)\n"
        "  -H HOST    InfluxDB host (default 192.168.50.1)\n"
        "  -P PORT    InfluxDB port (default 8086)\n"
        "  -O ORG     org (default qut-microgrid)\n"
        "  -b BUCKET  bucket (default telemetry)\n"
        "  -k FILE    token file (default /mnt/sd/influx.token, or $INFLUX_TOKEN)\n"
        "  -n NODE    node_id tag (default $NODE_ID or cosmos-test)\n"
        "  -s ID      session tag (default burst-<unix time>; try -s $(philics-runinc))\n"
        "  -t TAG     free-text tag, e.g. sine50 (default none)\n"
        "  -M MEAS    measurement (default burst; summary goes to MEAS_meta)\n"
        "  -B LINES   lines per POST (default 5000)\n"
        "  -w FILE    write line protocol to FILE instead of posting (use /tmp)\n"
        "  -g CONF    gains file (default /mnt/sd/adcmon.conf)\n"
        "  -m DEV     memory device (default /dev/mem)\n"
        "  -p US      poll period (default 1000)\n"
        "Run adcmon start (or labup) first.\n", MAX_DUR);
}

int main(int argc, char **argv)
{
    const char *envnode = getenv("NODE_ID");
    struct opts o = {
        .dev = "/dev/mem", .conf = "/mnt/sd/adcmon.conf", .host = "192.168.50.1",
        .port = "8086", .org = "qut-microgrid", .bucket = "telemetry",
        .tokfile = "/mnt/sd/influx.token", .node = envnode && *envnode ? envnode : "cosmos-test",
        .tag = "", .meas = "burst", .chmask = 0x3, .duration = 1, .poll_us = 1000, .batch = 5000,
    };
    char sessbuf[64];
    int opt;
    while ((opt = getopt(argc, argv, "d:c:H:P:O:b:k:n:s:t:M:B:w:g:m:p:h")) != -1) {
        switch (opt) {
        case 'd': o.duration = atof(optarg); break;
        case 'c': o.chmask = parse_mask(optarg); break;
        case 'H': o.host = optarg; break;
        case 'P': o.port = optarg; break;
        case 'O': o.org = optarg; break;
        case 'b': o.bucket = optarg; break;
        case 'k': o.tokfile = optarg; break;
        case 'n': o.node = optarg; break;
        case 's': o.session = optarg; break;
        case 't': o.tag = optarg; break;
        case 'M': o.meas = optarg; break;
        case 'B': o.batch = atol(optarg); break;
        case 'w': o.wfile = optarg; break;
        case 'g': o.conf = optarg; break;
        case 'm': o.dev = optarg; break;
        case 'p': o.poll_us = atol(optarg); break;
        case 'h': usage(); return 0;
        default: usage(); return 2;
        }
    }
    if (optind < argc || o.duration <= 0 || o.duration > MAX_DUR || o.batch < 1 || o.batch > 50000) {
        usage(); return 2;
    }
    if (o.wfile && !strncmp(o.wfile, "/mnt/sd", 7))
        fprintf(stderr, "adcburst: note: writing to the SD card is slow; /tmp is faster\n");
    if (!o.session) {
        snprintf(sessbuf, sizeof sessbuf, "burst-%" PRIu64, (uint64_t)(now_ns(CLOCK_REALTIME) / 1000000000ull));
        o.session = sessbuf;
    }
    if (now_ns(CLOCK_REALTIME) < 1700000000ull * 1000000000ull) {
        fprintf(stderr, "adcburst: system clock not set; set it before capturing\n");
        return 1;
    }
    if (!o.wfile) {
        o.token = read_token(&o);
        if (!o.token) {
            fprintf(stderr, "adcburst: no token (put it in %s or set INFLUX_TOKEN)\n", o.tokfile);
            return 1;
        }
    }
    load_gains(&o);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    struct ring r = { 0 };
    r.p = map_phys(o.dev, RING_BASE, NREC * STRIDE);
    if (!r.p) return 1;
    if (ring_attach(&r) < 0) {
        fprintf(stderr, "adcburst: no consistent write head; is the DAQ running? (run: adcmon start)\n");
        return 1;
    }

    fprintf(stderr, "adcburst: capturing %.3f s, channels 0x%X, session %s\n",
            o.duration, o.chmask, o.session);
    struct capture cp = { 0 };
    if (capture(&o, &r, &cp)) { fprintf(stderr, "adcburst: no samples captured\n"); return 1; }
    double span = cp.n > 1 ? (double)(cp.s[cp.n - 1].ts - cp.s[0].ts) / TICK_HZ : 0;
    fprintf(stderr, "adcburst: %zu samples over %.3f s (%.3f Hz), %" PRIu64 " missed, "
                    "anchor spread %" PRId64 " ns over %ld polls\n",
            cp.n, span, span > 0 ? (cp.n - 1) / span : 0, cp.missed,
            cp.off_max - cp.off_min, cp.polls);
    if (stop) { fprintf(stderr, "adcburst: interrupted; nothing uploaded\n"); return 1; }

    if (upload(&o, &cp, cp.off_min)) return 1;
    return cp.missed ? 3 : 0;
}
