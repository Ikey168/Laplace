/* Laplace core: 16550 serial driver, kernel console, and panic (#219). */

#include "core/console.h"
#include "core/cpu.h"

/* 16550 register offsets. */
#define UART_DATA  0
#define UART_IER   1
#define UART_FCR   2
#define UART_LCR   3
#define UART_MCR   4
#define UART_LSR   5
#define UART_SCR   7

#define LSR_DATA_READY  0x01
#define LSR_THR_EMPTY   0x20

void serial_init(uint16_t port) {
    outb(port + UART_IER, 0x00);   /* no UART interrupts: everything is polled */
    outb(port + UART_LCR, 0x80);   /* DLAB on */
    outb(port + UART_DATA, 0x01);  /* divisor 1: 115200 baud */
    outb(port + UART_IER, 0x00);
    outb(port + UART_LCR, 0x03);   /* 8N1, DLAB off */
    outb(port + UART_FCR, 0xC7);   /* FIFO on, cleared, 14-byte threshold */
    outb(port + UART_MCR, 0x03);   /* DTR | RTS */
}

bool serial_present(uint16_t port) {
    /* The scratch register reads back what was written on a real 16550. */
    outb(port + UART_SCR, 0xA5);
    if (inb(port + UART_SCR) != 0xA5) return false;
    outb(port + UART_SCR, 0x5A);
    return inb(port + UART_SCR) == 0x5A;
}

void serial_putc(uint16_t port, char c) {
    while ((inb(port + UART_LSR) & LSR_THR_EMPTY) == 0) { }
    outb(port + UART_DATA, (uint8_t)c);
}

bool serial_rx_ready(uint16_t port) {
    return (inb(port + UART_LSR) & LSR_DATA_READY) != 0;
}

int serial_getc(uint16_t port) {
    while (!serial_rx_ready(port)) { }
    return inb(port + UART_DATA);
}

/* ---- Console ---- */

void console_write(const char* s, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        if (s[i] == '\n') serial_putc(PORT_CONSOLE, '\r');
        serial_putc(PORT_CONSOLE, s[i]);
    }
}

void kputs(const char* s) {
    uint32_t n = 0;
    while (s[n]) n++;
    console_write(s, n);
}

/* ---- Formatting ---- */

typedef struct {
    char*    buf;      /* NULL: write to the console */
    uint32_t cap;
    uint32_t len;
} out_t;

static void out_char(out_t* o, char c) {
    if (!o->buf) {
        console_write(&c, 1);
    } else if (o->len + 1 < o->cap) {
        o->buf[o->len] = c;
    }
    o->len++;
}

static void out_num(out_t* o, uint64_t v, unsigned base, bool upper, bool neg,
                    int width, char pad) {
    char tmp[24];
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);
    int total = n + (neg ? 1 : 0);
    if (neg && pad == '0') out_char(o, '-');
    for (int i = total; i < width; i++) out_char(o, pad);
    if (neg && pad != '0') out_char(o, '-');
    while (n) out_char(o, tmp[--n]);
}

static void format(out_t* o, const char* fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') { out_char(o, *fmt); continue; }
        fmt++;
        char pad = ' ';
        int width = 0;
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        int lng = 0;
        while (*fmt == 'l') { lng++; fmt++; }
        switch (*fmt) {
        case 'd': case 'i': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            bool neg = v < 0;
            out_num(o, neg ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v, 10, false, neg, width, pad);
            break;
        }
        case 'u': {
            uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            out_num(o, v, 10, false, false, width, pad);
            break;
        }
        case 'x': case 'X': {
            uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            out_num(o, v, 16, *fmt == 'X', false, width, pad);
            break;
        }
        case 'p': {
            uint64_t v = (uint64_t)va_arg(ap, void*);
            out_char(o, '0'); out_char(o, 'x');
            out_num(o, v, 16, false, false, 16, '0');
            break;
        }
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            int n = 0;
            while (s[n]) n++;
            for (int i = n; i < width; i++) out_char(o, ' ');
            for (int i = 0; i < n; i++) out_char(o, s[i]);
            break;
        }
        case 'c':
            out_char(o, (char)va_arg(ap, int));
            break;
        case '%':
            out_char(o, '%');
            break;
        case '\0':
            return;
        default:
            out_char(o, '%');
            out_char(o, *fmt);
            break;
        }
    }
}

void kvprintf(const char* fmt, va_list ap) {
    out_t o = { 0, 0, 0 };
    format(&o, fmt, ap);
}

void kprintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

int ksnprintf(char* buf, uint32_t cap, const char* fmt, ...) {
    out_t o = { buf, cap, 0 };
    va_list ap;
    va_start(ap, fmt);
    format(&o, fmt, ap);
    va_end(ap);
    if (cap) buf[o.len < cap ? o.len : cap - 1] = '\0';
    return (int)o.len;
}

void panic(const char* fmt, ...) {
    __asm__ volatile("cli");
    kputs("\n*** Laplace kernel panic: ");
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kputs("\n");
    cpu_halt_forever();
}
