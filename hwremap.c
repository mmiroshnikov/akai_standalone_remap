// hwremap: LD_PRELOAD shim for MPC OS standalone that turns hardware buttons into
// button/pad sequences. It filters the controller's "Private" rawmidi input
// (buttons = note on/off on channel 1, pads = notes on channel 10).
//
// Config (re-read whenever its mtime changes): /sdcard/hwremap.conf
//   log 1                     log every input byte to /tmp/hwremap.log
//   hold 49                   while this button is held, remapped buttons pass through
//   54 d123 p49 u123          on press of note 54: Menu down, tap pad note 49, Menu up
//   54 mClip_Matrix           Menu + the pad under that mode in the Mode Menu grid
//   dbl 55 b116               second press within dblms (default 350) runs this instead
//   dblms 350
// Tokens: dN button down, uN button up, bN button tap, pN pad tap, xHH raw byte,
// mName (underscores = spaces; looked up in MPC.settings at press time, used alone).
// N is decimal or 0x hex. The source button's release is swallowed.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

typedef struct _snd_rawmidi snd_rawmidi_t;
typedef struct _snd_rawmidi_info snd_rawmidi_info_t;

#ifndef CONF_PATH
#define CONF_PATH "/sdcard/hwremap.conf"
#endif
#ifndef LOG_PATH
#define LOG_PATH "/tmp/hwremap.log"
#endif
#ifndef SETTINGS_PATH
#define SETTINGS_PATH "/media/az01-internal/Settings/MPC/MPC.settings"
#endif
#define MENU_NOTE 123
#define MENU_GRID_PAGE "ModeMenuPage1Slot"
#define MODE_NAME_MAX 48
#define LOG_MAX (256 * 1024)
#define MAX_SEQ 64
#define QSIZE 4096

static int (*real_open)(snd_rawmidi_t **, snd_rawmidi_t **, const char *, int);
static ssize_t (*real_read)(snd_rawmidi_t *, void *, size_t);

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static snd_rawmidi_t *priv_in;

struct action {
    unsigned char seq[MAX_SEQ];
    int len;
    char mode[MODE_NAME_MAX];
};

static struct action single[128], dbl[128];
static long long last_press[128];
static int dbl_ms = 350;

// pad note for each Mode Menu grid slot: slot 0 is top left (pad 13), rows go down
static const unsigned char slot_pad[16] = {
    0x31, 0x37, 0x33, 0x35, // pads 13-16
    0x30, 0x2F, 0x2D, 0x2B, // pads 9-12
    0x28, 0x26, 0x2E, 0x2C, // pads 5-8
    0x25, 0x24, 0x2A, 0x52, // pads 1-4
};

static char grid[16][MODE_NAME_MAX];
static time_t settings_mtime = -1;
static long settings_mtime_ns = -1;
static int hold_note = -1;
static int log_on;
static time_t conf_mtime = -1;
static long conf_mtime_ns = -1;

static int held[128];
static int swallow_release[128];

static unsigned char q[QSIZE];
static int qhead, qtail;

static unsigned char msg[3];
static int msg_len, msg_need, in_sysex;

static void logf_(const char *fmt, ...)
{
    if (!log_on)
        return;
    struct stat st;
    if (stat(LOG_PATH, &st) == 0 && st.st_size > LOG_MAX)
        rename(LOG_PATH, LOG_PATH ".1");
    FILE *f = fopen(LOG_PATH, "a");
    if (!f)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

static void push(unsigned char b)
{
    int next = (qtail + 1) % QSIZE;
    if (next == qhead)
        return;
    q[qtail] = b;
    qtail = next;
}

static int add_tok(struct action *a, const char *tok)
{
    unsigned char *p = a->seq + a->len;
    int room = MAX_SEQ - a->len;
    char kind = tok[0];
    if (kind == 'm') {
        snprintf(a->mode, MODE_NAME_MAX, "%s", tok + 1);
        for (char *c = a->mode; *c; c++)
            if (*c == '_')
                *c = ' ';
        return a->mode[0] ? 0 : -1;
    }
    long v = strtol(tok + 1, NULL, kind == 'x' ? 16 : 0);
    if (v < 0 || v > (kind == 'x' ? 255 : 127))
        return -1;
    int n = 0;
    unsigned char b[6];
    switch (kind) {
    case 'd': b[0] = 0x90; b[1] = v; b[2] = 0x7f; n = 3; break;
    case 'u': b[0] = 0x90; b[1] = v; b[2] = 0x00; n = 3; break;
    case 'b': b[0] = 0x90; b[1] = v; b[2] = 0x7f; b[3] = 0x90; b[4] = v; b[5] = 0x00; n = 6; break;
    case 'p': b[0] = 0x99; b[1] = v; b[2] = 0x7f; b[3] = 0x99; b[4] = v; b[5] = 0x00; n = 6; break;
    case 'x': b[0] = v; n = 1; break;
    default: return -1;
    }
    if (n > room)
        return -1;
    memcpy(p, b, n);
    a->len += n;
    return 0;
}

static void reset_conf(void)
{
    memset(single, 0, sizeof single);
    memset(dbl, 0, sizeof dbl);
    hold_note = -1;
    log_on = 0;
    dbl_ms = 350;
}

static void load_conf(void)
{
    struct stat st;
    if (stat(CONF_PATH, &st) != 0) {
        if (conf_mtime != 0) {
            reset_conf();
            conf_mtime = 0;
        }
        return;
    }
    if (st.st_mtim.tv_sec == conf_mtime && st.st_mtim.tv_nsec == conf_mtime_ns)
        return;
    conf_mtime = st.st_mtim.tv_sec;
    conf_mtime_ns = st.st_mtim.tv_nsec;

    FILE *f = fopen(CONF_PATH, "r");
    if (!f)
        return;
    reset_conf();
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        char *save = NULL;
        char *w = strtok_r(line, " \t\r\n", &save);
        if (!w)
            continue;
        if (!strcmp(w, "log")) {
            char *v = strtok_r(NULL, " \t\r\n", &save);
            log_on = v && atoi(v);
            continue;
        }
        if (!strcmp(w, "hold")) {
            char *v = strtok_r(NULL, " \t\r\n", &save);
            hold_note = v ? (int)strtol(v, NULL, 0) : -1;
            continue;
        }
        if (!strcmp(w, "dblms")) {
            char *v = strtok_r(NULL, " \t\r\n", &save);
            dbl_ms = v ? atoi(v) : 350;
            continue;
        }
        struct action *table = single;
        if (!strcmp(w, "dbl")) {
            table = dbl;
            w = strtok_r(NULL, " \t\r\n", &save);
            if (!w)
                continue;
        }
        long src = strtol(w, NULL, 0);
        if (src < 0 || src > 127)
            continue;
        struct action *a = &table[src];
        memset(a, 0, sizeof *a);
        for (char *t = strtok_r(NULL, " \t\r\n", &save); t; t = strtok_r(NULL, " \t\r\n", &save)) {
            if (add_tok(a, t) != 0) {
                logf_("bad token '%s' for %ld\n", t, src);
                break;
            }
        }
    }
    fclose(f);
    logf_("config loaded\n");
}

static void load_grid(void)
{
    struct stat st;
    if (stat(SETTINGS_PATH, &st) != 0)
        return;
    if (st.st_mtim.tv_sec == settings_mtime && st.st_mtim.tv_nsec == settings_mtime_ns)
        return;
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f)
        return;
    char *buf = malloc((size_t)st.st_size + 1);
    size_t n = buf ? fread(buf, 1, (size_t)st.st_size, f) : 0;
    fclose(f);
    if (!buf)
        return;
    buf[n] = 0;
    settings_mtime = st.st_mtim.tv_sec;
    settings_mtime_ns = st.st_mtim.tv_nsec;
    memset(grid, 0, sizeof grid);
    static const char key[] = "<VALUE name=\"" MENU_GRID_PAGE;
    for (char *k = strstr(buf, key); k; k = strstr(k + 1, key)) {
        char *end;
        long slot = strtol(k + sizeof key - 1, &end, 10);
        if (slot < 0 || slot >= 16 || strncmp(end, "\" val=\"", 7))
            continue;
        char *v = end + 7;
        char *q2 = strchr(v, '"');
        if (!q2)
            continue;
        int len = (int)(q2 - v) < MODE_NAME_MAX - 1 ? (int)(q2 - v) : MODE_NAME_MAX - 1;
        memcpy(grid[slot], v, len);
        grid[slot][len] = 0;
    }
    free(buf);
    logf_("menu grid loaded\n");
}

static int push_mode(const char *name)
{
    load_grid();
    for (int slot = 0; slot < 16; slot++) {
        if (strcmp(grid[slot], name))
            continue;
        unsigned char pad = slot_pad[slot];
        const unsigned char s[] = {0x90, MENU_NOTE, 0x7f, 0x99, pad, 0x7f, 0x99, pad, 0x00, 0x90, MENU_NOTE, 0x00};
        for (size_t i = 0; i < sizeof s; i++)
            push(s[i]);
        logf_("mode '%s' -> slot %d\n", name, slot);
        return 1;
    }
    logf_("mode '%s' not in the menu grid\n", name);
    return 0;
}

static int mapped(const struct action *a)
{
    return a->len || a->mode[0];
}

static int run_action(const struct action *a)
{
    if (a->mode[0] && !push_mode(a->mode))
        return 0;
    for (int i = 0; i < a->len; i++)
        push(a->seq[i]);
    return 1;
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void handle_msg(void)
{
    int st = msg[0] & 0xf0, ch = msg[0] & 0x0f;
    if (ch == 0 && (st == 0x90 || st == 0x80) && msg_len == 3) {
        int note = msg[1];
        int press = st == 0x90 && msg[2] > 0;
        load_conf();
        if (press && mapped(&single[note]) && !(hold_note >= 0 && held[hold_note])) {
            long long t = now_ms();
            const struct action *a = &single[note];
            if (mapped(&dbl[note]) && last_press[note] && t - last_press[note] <= dbl_ms) {
                a = &dbl[note];
                last_press[note] = 0;
            } else {
                last_press[note] = t;
            }
            if (run_action(a)) {
                swallow_release[note] = 1;
                logf_("remap %d%s\n", note, a == &dbl[note] ? " (double)" : "");
                return;
            }
        }
        if (!press && swallow_release[note]) {
            swallow_release[note] = 0;
            return;
        }
        held[note] = press;
    }
    for (int i = 0; i < msg_len; i++)
        push(msg[i]);
}

static int data_len(unsigned char s)
{
    switch (s & 0xf0) {
    case 0xc0:
    case 0xd0:
        return 1;
    case 0xf0:
        return s == 0xf1 || s == 0xf3 ? 1 : s == 0xf2 ? 2 : 0;
    default:
        return 2;
    }
}

static void feed(unsigned char b)
{
    if (b >= 0xf8) {
        push(b);
        return;
    }
    if (b == 0xf0) {
        in_sysex = 1;
        msg[0] = 0;
        msg_len = 0;
        push(b);
        return;
    }
    if (in_sysex) {
        if (b == 0xf7 || !(b & 0x80)) {
            push(b);
            if (b == 0xf7)
                in_sysex = 0;
            return;
        }
        in_sysex = 0;
    }
    if (b & 0x80) {
        msg[0] = b;
        msg_len = 1;
        msg_need = data_len(b);
        if (msg_need == 0) {
            push(b);
            msg_len = 0;
        }
        return;
    }
    if (msg_len == 0) {
        if (msg[0] < 0x80 || msg[0] >= 0xf0) {
            push(b);
            return;
        }
        msg_len = 1;
    }
    msg[msg_len++] = b;
    if (msg_len == msg_need + 1) {
        handle_msg();
        msg_len = 0;
    }
}

static void init_syms(void)
{
    if (!real_open)
        real_open = dlsym(RTLD_NEXT, "snd_rawmidi_open");
    if (!real_read)
        real_read = dlsym(RTLD_NEXT, "snd_rawmidi_read");
}

static int is_private(snd_rawmidi_t *h, const char *name)
{
    size_t (*info_sizeof)(void) = dlsym(RTLD_DEFAULT, "snd_rawmidi_info_sizeof");
    int (*info)(snd_rawmidi_t *, snd_rawmidi_info_t *) = dlsym(RTLD_DEFAULT, "snd_rawmidi_info");
    const char *(*subname)(const snd_rawmidi_info_t *) =
        dlsym(RTLD_DEFAULT, "snd_rawmidi_info_get_subdevice_name");
    const char *(*devname)(const snd_rawmidi_info_t *) = dlsym(RTLD_DEFAULT, "snd_rawmidi_info_get_name");
    int found = 0;
    if (info_sizeof && info && subname && devname) {
        snd_rawmidi_info_t *ri = calloc(1, info_sizeof());
        if (ri && info(h, ri) == 0) {
            const char *sn = subname(ri), *dn = devname(ri);
            logf_("open %s: '%s' / '%s'\n", name ? name : "?", dn ? dn : "", sn ? sn : "");
            found = (sn && strstr(sn, "Private")) || (dn && strstr(dn, "Private"));
        }
        free(ri);
    }
    return found;
}

__attribute__((visibility("default"))) int snd_rawmidi_open(snd_rawmidi_t **in, snd_rawmidi_t **out,
                                                             const char *name, int mode)
{
    init_syms();
    int r = real_open(in, out, name, mode);
    if (r == 0 && in && *in) {
        pthread_mutex_lock(&lock);
        load_conf();
        if (is_private(*in, name)) {
            priv_in = *in;
            qhead = qtail = 0;
            msg_len = 0;
            in_sysex = 0;
            memset(held, 0, sizeof held);
            memset(swallow_release, 0, sizeof swallow_release);
            logf_("hooked private input %p\n", (void *)priv_in);
        }
        pthread_mutex_unlock(&lock);
    }
    return r;
}

__attribute__((visibility("default"))) ssize_t snd_rawmidi_read(snd_rawmidi_t *h, void *buffer, size_t size)
{
    init_syms();
    if (h != priv_in || size == 0)
        return real_read(h, buffer, size);

    unsigned char *out = buffer;
    size_t n = 0;
    pthread_mutex_lock(&lock);
    while (n < size && qhead != qtail) {
        out[n++] = q[qhead];
        qhead = (qhead + 1) % QSIZE;
    }
    while (n == 0) {
        unsigned char tmp[256];
        size_t want = size < sizeof tmp ? size : sizeof tmp;
        pthread_mutex_unlock(&lock);
        ssize_t r = real_read(h, tmp, want);
        pthread_mutex_lock(&lock);
        if (r <= 0) {
            pthread_mutex_unlock(&lock);
            return r;
        }
        if (log_on) {
            char hex[3 * 256 + 1];
            for (ssize_t i = 0; i < r; i++)
                sprintf(hex + 3 * i, "%02X ", tmp[i]);
            logf_("in: %s\n", hex);
        }
        for (ssize_t i = 0; i < r; i++)
            feed(tmp[i]);
        while (n < size && qhead != qtail) {
            out[n++] = q[qhead];
            qhead = (qhead + 1) % QSIZE;
        }
    }
    pthread_mutex_unlock(&lock);
    return (ssize_t)n;
}
