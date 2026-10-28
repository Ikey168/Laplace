/* Laplace core: the console's input side, a minimal tty (#249).
 * See include/core/tty.h. */

#include "core/tty.h"
#include "core/kio.h"
#include "core/kstate.h"
#include "core/files.h"
#include "core/linux.h"
#include "core/machine.h"
#include "core/console.h"
#include "core/vm.h"
#include "core/mm.h"
#include "core/ksig.h"
#include "iorec.h"
#include "divergence.h"

void* memset(void* dst, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);

/* ---- Outside the machine: what the UART received, not yet delivered ---- */

#define RX_CAP 4096

static uint8_t  g_rx[RX_CAP];
static uint32_t g_rx_n;

void tty_poll_uart(void) {
    while (serial_rx_ready(PORT_CONSOLE)) {
        uint8_t c = (uint8_t)serial_getc(PORT_CONSOLE);
        if (g_rx_n < RX_CAP) g_rx[g_rx_n++] = c;
    }
}

bool tty_pending(void) { return g_rx_n != 0; }

void tty_discard_pending(void) {
    while (serial_rx_ready(PORT_CONSOLE)) (void)serial_getc(PORT_CONSOLE);
    g_rx_n = 0;
}

/* ---- Inside the machine ---- */

/* struct termios as the kernel's TCGETS returns it. */
typedef struct {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t  line;
    uint8_t  cc[19];
} __attribute__((packed)) ktermios_t;

#define T_ICRNL  0000400u
#define T_ICANON 0000002u
#define T_ECHO   0000010u
#define T_ECHOE  0000020u
#define T_ECHOK  0000040u
#define T_ISIG   0000001u
#define V_INTR  0
#define V_QUIT  1
#define V_ERASE 2
#define V_KILL  3
#define V_EOF   4

#define Q_CAP    16384
#define SEG_MAX  256
#define LINE_CAP 4096

/* Readable bytes in segments: a canonical read returns at most one segment
 * (a line, or what ^D committed; an empty one is an end of file). */
typedef struct {
    ktermios_t tio;
    uint32_t   qlen;
    uint32_t   nseg;
    uint32_t   seg[SEG_MAX];
    uint32_t   linelen;
    uint8_t    q[Q_CAP];
    uint8_t    line[LINE_CAP];
} tty_t;

static tty_t g_tty;

static void defaults(void) {
    memset(&g_tty, 0, sizeof(g_tty));
    g_tty.tio.iflag = T_ICRNL | 0002000;                     /* ICRNL IXON */
    g_tty.tio.oflag = 0000005;                               /* OPOST ONLCR */
    g_tty.tio.cflag = 0000277;                               /* B38400 CS8 CREAD */
    g_tty.tio.lflag = T_ISIG | T_ICANON | T_ECHO | T_ECHOE | T_ECHOK | 0001000 | 0004000 | 0100000;
    static const uint8_t cc[19] = { 3, 034, 0177, 025, 4, 0, 1, 0, 021, 023, 032, 0, 022, 017, 027, 026 };
    memcpy(g_tty.tio.cc, cc, sizeof(cc));
}

static void echo(const char* s, uint32_t n) {
    if (g_tty.tio.lflag & T_ECHO) machine_device_write(MACHINE_DEV_CONSOLE, s, n);
}

/* Commit `n` bytes as one readable segment (n == 0: an end of file). */
static void commit(const uint8_t* b, uint32_t n) {
    if (g_tty.nseg == SEG_MAX || g_tty.qlen + n > Q_CAP) return;   /* full: dropped */
    memcpy(g_tty.q + g_tty.qlen, b, n);
    g_tty.qlen += n;
    g_tty.seg[g_tty.nseg++] = n;
}

static void input_byte(uint8_t c) {
    ktermios_t* t = &g_tty.tio;
    if (c == '\r' && (t->iflag & T_ICRNL)) c = '\n';
    if ((t->lflag & T_ISIG) && (c == t->cc[V_INTR] || c == t->cc[V_QUIT])) {
        echo(c == t->cc[V_INTR] ? "^C\n" : "^\\\n", 3);
        g_tty.linelen = 0;
        files_console_signal(c == t->cc[V_INTR] ? SIG_SIGINT : 3);
        return;
    }
    if (!(t->lflag & T_ICANON)) {
        commit(&c, 1);
        char e = (char)c;
        echo(&e, 1);
        return;
    }
    if (c == t->cc[V_ERASE] || c == '\b') {
        if (g_tty.linelen) {
            g_tty.linelen--;
            if (t->lflag & T_ECHOE) echo("\b \b", 3);
        }
        return;
    }
    if (c == t->cc[V_KILL]) {
        while (g_tty.linelen) {
            g_tty.linelen--;
            if (t->lflag & T_ECHOK) echo("\b \b", 3);
        }
        return;
    }
    if (c == t->cc[V_EOF]) {
        commit(g_tty.line, g_tty.linelen);
        g_tty.linelen = 0;
        return;
    }
    if (g_tty.linelen < LINE_CAP) g_tty.line[g_tty.linelen++] = c;
    char e = (char)c;
    echo(&e, 1);
    if (c == '\n' || g_tty.linelen == LINE_CAP) {
        commit(g_tty.line, g_tty.linelen);
        g_tty.linelen = 0;
    }
}

static void input(const uint8_t* b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) input_byte(b[i]);
}

#define IDLE_FLAG (1ULL << 63)

bool tty_deliver(uint64_t step, bool idle) {
    uint8_t buf[RX_CAP];
    if (machine_mode() == MACHINE_LIVE) {
        if (!g_rx_n) return false;
        uint32_t n = g_rx_n;
        memcpy(buf, g_rx, n);
        g_rx_n = 0;
        kio_record(IOREC_CONSOLE, step, n | (idle ? IDLE_FLAG : 0), buf, n);
        input(buf, n);
        return true;
    }
    uint64_t args;
    uint32_t len;
    if (!kio_record_peek(IOREC_CONSOLE, step, &args) || ((args & IDLE_FLAG) != 0) != idle) return false;
    if (!kio_record_next(IOREC_CONSOLE, step, &args, buf, sizeof(buf), &len)) return false;
    input(buf, len);
    return true;
}

bool tty_readable(void) { return g_tty.nseg != 0; }

static void take(uint32_t n) {
    memmove(g_tty.q, g_tty.q + n, g_tty.qlen - n);
    g_tty.qlen -= n;
}

int64_t tty_read(process_t* p, uint64_t buf, uint64_t len) {
    if (!g_tty.nseg) return -(int64_t)LX_EAGAIN;
    if (!len) return 0;
    uint32_t n;
    if (g_tty.tio.lflag & T_ICANON) {
        /* One segment, or the part of it that fits. */
        n = g_tty.seg[0] < len ? g_tty.seg[0] : (uint32_t)len;
        if (n && !uaccess_write(p->address_space, buf, g_tty.q, n)) return -(int64_t)LX_EFAULT;
        take(n);
        if ((g_tty.seg[0] -= n) == 0 || n == 0) {
            memmove(g_tty.seg, g_tty.seg + 1, (g_tty.nseg - 1) * sizeof(uint32_t));
            g_tty.nseg--;
        }
        return n;
    }
    n = g_tty.qlen < len ? g_tty.qlen : (uint32_t)len;
    if (n && !uaccess_write(p->address_space, buf, g_tty.q, n)) return -(int64_t)LX_EFAULT;
    take(n);
    /* Drop the segments it consumed. */
    for (uint32_t left = n; left && g_tty.nseg;) {
        if (g_tty.seg[0] > left) {
            g_tty.seg[0] -= left;
            break;
        }
        left -= g_tty.seg[0];
        memmove(g_tty.seg, g_tty.seg + 1, (g_tty.nseg - 1) * sizeof(uint32_t));
        g_tty.nseg--;
    }
    return n;
}

bool tty_ioctl(process_t* p, uint64_t req, uint64_t arg, int64_t* ret) {
    vm_space_t* as = p->address_space;
    switch (req) {
    case 0x5401:                                         /* TCGETS */
        *ret = uaccess_write(as, arg, &g_tty.tio, sizeof(g_tty.tio)) ? 0 : -(int64_t)LX_EFAULT;
        return true;
    case 0x5402: case 0x5403: case 0x5404: {             /* TCSETS, TCSETSW, TCSETSF */
        ktermios_t t;
        if (!uaccess_read(as, arg, &t, sizeof(t))) {
            *ret = -(int64_t)LX_EFAULT;
            return true;
        }
        bool was_canon = (g_tty.tio.lflag & T_ICANON) != 0;
        g_tty.tio = t;
        if (req == 0x5404) {                             /* flush input */
            g_tty.qlen = g_tty.nseg = g_tty.linelen = 0;
        } else if (was_canon && !(t.lflag & T_ICANON) && g_tty.linelen) {
            commit(g_tty.line, g_tty.linelen);           /* the partial line becomes readable */
            g_tty.linelen = 0;
        }
        *ret = 0;
        return true;
    }
    case 0x5413: {                                       /* TIOCGWINSZ: 24 x 80 */
        uint16_t ws[4] = { 24, 80, 0, 0 };
        *ret = uaccess_write(as, arg, ws, sizeof(ws)) ? 0 : -(int64_t)LX_EFAULT;
        return true;
    }
    case 0x5414:                                         /* TIOCSWINSZ */
        *ret = 0;
        return true;
    case 0x540F: case 0x5429: {                          /* TIOCGPGRP, TIOCGSID */
        int32_t pg = (int32_t)p->tgid;
        *ret = uaccess_write(as, arg, &pg, 4) ? 0 : -(int64_t)LX_EFAULT;
        return true;
    }
    case 0x5410: case 0x540E: case 0x540B:               /* TIOCSPGRP, TIOCSCTTY, TCFLSH */
        if (req == 0x540B && (arg == 0 || arg == 2)) g_tty.qlen = g_tty.nseg = g_tty.linelen = 0;
        *ret = 0;
        return true;
    case 0x5409: case 0x540A:                            /* TCSBRK, TCXONC */
        *ret = 0;
        return true;
    case 0x541B: {                                       /* FIONREAD */
        int32_t n = (int32_t)g_tty.qlen;
        *ret = uaccess_write(as, arg, &n, 4) ? 0 : -(int64_t)LX_EFAULT;
        return true;
    }
    default:
        return false;
    }
}

/* ---- Keyframes ---- */

static void save(ksf_writer_t* w, void* ctx) {
    (void)ctx;
    ksf_put(w, &g_tty.tio, sizeof(g_tty.tio));
    ksf_put_u32(w, g_tty.nseg);
    for (uint32_t i = 0; i < g_tty.nseg; i++) ksf_put_u32(w, g_tty.seg[i]);
    ksf_put_u32(w, g_tty.qlen);
    ksf_put(w, g_tty.q, g_tty.qlen);
    ksf_put_u32(w, g_tty.linelen);
    ksf_put(w, g_tty.line, g_tty.linelen);
}

static int restore(ksf_reader_t* r, uint32_t version, void* ctx) {
    (void)ctx;
    if (version != 1) return -1;
    defaults();
    ksf_get(r, &g_tty.tio, sizeof(g_tty.tio));
    g_tty.nseg = ksf_get_u32(r);
    if (g_tty.nseg > SEG_MAX) return -1;
    for (uint32_t i = 0; i < g_tty.nseg; i++) g_tty.seg[i] = ksf_get_u32(r);
    g_tty.qlen = ksf_get_u32(r);
    if (g_tty.qlen > Q_CAP) return -1;
    ksf_get(r, g_tty.q, g_tty.qlen);
    g_tty.linelen = ksf_get_u32(r);
    if (g_tty.linelen > LINE_CAP) return -1;
    ksf_get(r, g_tty.line, g_tty.linelen);
    return r->ok ? 0 : -1;
}

static uint32_t checksum(void* ctx) {
    (void)ctx;
    uint32_t crc = divergence_checksum(0, &g_tty.tio, sizeof(g_tty.tio));
    crc = divergence_checksum(crc, g_tty.seg, g_tty.nseg * sizeof(uint32_t));
    crc = divergence_checksum(crc, g_tty.q, g_tty.qlen);
    crc = divergence_checksum(crc, &g_tty.linelen, sizeof(g_tty.linelen));
    return divergence_checksum(crc, g_tty.line, g_tty.linelen);
}

void tty_init(void) {
    defaults();
    static const ksf_subsys_t sub = { KS_ID_TTY, "tty", 1, save, restore, checksum, 0 };
    if (kstate_register(&sub) != KSF_OK) panic("tty: cannot register its keyframe section");
}
