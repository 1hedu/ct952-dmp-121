/*
 * ct952emu -- GDB remote-serial-protocol (RSP) stub.
 *
 * Lets a real gdb drive the emulated CT909 PROC1: breakpoints, single-step,
 * register + memory inspection of the live firmware. Connect with a SPARC gdb
 * (or gdb-multiarch):
 *
 *     ./ct952emu --rom-load --gdb 3333 --quiet dp700wd.bin
 *     sparc64-linux-gnu-gdb -ex 'set architecture sparc' \
 *         -ex 'target remote :3333'
 *
 * The stub speaks the minimum RSP a modern gdb needs: qSupported, ?, g/G,
 * p/P, m/M, c/s, Z0/z0 (breakpoints), plus the harmless query acks. Ctrl-C in
 * gdb (a bare 0x03 on the wire) interrupts a running target. Breakpoints are
 * stub-managed (an address list checked each instruction) -- no code patching,
 * so they work in XIP flash as well as DRAM.
 *
 * SPARC 'g'-packet register order (72 regs x 4 bytes, big-endian):
 *   0..7  g0-g7   8..15 o0-o7   16..23 l0-l7   24..31 i0-i7
 *   32..63 f0-f31 (soft-float target: reported 0)
 *   64 y  65 psr  66 wim  67 tbr  68 pc  69 npc  70 fsr  71 csr
 */
#include "machine.h"
#include "sparc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define GDB_NREG      72
#define GDB_MAXBP     64
#define GDB_BATCH     2000000ull   /* instrs per continue-batch between socket polls */
#define PKT_MAX       (16 * 1024)

static const char hexd[] = "0123456789abcdef";

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* --- register file <-> gdb reg number ------------------------------------ */

static uint32_t gdb_getreg(machine_t *m, int n)
{
    if (n >= 0 && n < 32) return sparc_get_reg(&m->cpu, n);
    switch (n) {
    case 64: return m->cpu.y;
    case 65: return m->cpu.psr;
    case 66: return m->cpu.wim;
    case 67: return m->cpu.tbr;
    case 68: return m->cpu.pc;
    case 69: return m->cpu.npc;
    default: return 0;   /* f0-f31, fsr, csr */
    }
}

static void gdb_setreg(machine_t *m, int n, uint32_t v)
{
    if (n >= 0 && n < 32) { sparc_set_reg(&m->cpu, n, v); return; }
    switch (n) {
    case 64: m->cpu.y = v; break;
    case 65: m->cpu.psr = v; break;
    case 66: m->cpu.wim = v; break;
    case 67: m->cpu.tbr = v; break;
    case 68: m->cpu.pc = v; break;
    case 69: m->cpu.npc = v; break;
    default: break;
    }
}

/* --- packet I/O ---------------------------------------------------------- */

static int sock_getc(int fd)
{
    unsigned char c;
    ssize_t r;
    do { r = recv(fd, &c, 1, 0); } while (r < 0 && errno == EINTR);
    if (r <= 0) return -1;
    return c;
}

/* Read one RSP packet body into buf (NUL-terminated). Returns length, 0 for a
 * bare Ctrl-C (0x03) interrupt, or -1 on EOF/error. Sends the '+' ack. */
static int recv_packet(int fd, char *buf, size_t bufsz)
{
    int c;
    for (;;) {
        c = sock_getc(fd);
        if (c < 0) return -1;
        if (c == 0x03) return 0;         /* interrupt request */
        if (c == '$') break;             /* start of packet */
        /* swallow stray '+'/'-' acks */
    }
    size_t n = 0;
    unsigned char sum = 0;
    for (;;) {
        c = sock_getc(fd);
        if (c < 0) return -1;
        if (c == '#') break;
        if (n + 1 < bufsz) buf[n++] = (char)c;
        sum = (unsigned char)(sum + c);
    }
    int h = sock_getc(fd), l = sock_getc(fd);
    if (h < 0 || l < 0) return -1;
    buf[n] = '\0';
    (void)sum; (void)h; (void)l;   /* we trust the stream; always ack '+' */
    if (send(fd, "+", 1, 0) != 1) return -1;
    return (int)n;
}

static int send_packet(int fd, const char *data)
{
    static char out[PKT_MAX + 8];
    size_t len = strlen(data);
    unsigned char sum = 0;
    size_t i, o = 0;
    if (len > PKT_MAX) len = PKT_MAX;
    out[o++] = '$';
    for (i = 0; i < len; i++) { out[o++] = data[i]; sum += (unsigned char)data[i]; }
    out[o++] = '#';
    out[o++] = hexd[(sum >> 4) & 0xf];
    out[o++] = hexd[sum & 0xf];
    /* gdb replies '+'/'-'; we don't retransmit -- good enough on loopback */
    return send(fd, out, o, 0) == (ssize_t)o ? 0 : -1;
}

static void put_hex32_be(char *p, uint32_t v)   /* big-endian target order */
{
    p[0] = hexd[(v >> 28) & 0xf]; p[1] = hexd[(v >> 24) & 0xf];
    p[2] = hexd[(v >> 20) & 0xf]; p[3] = hexd[(v >> 16) & 0xf];
    p[4] = hexd[(v >> 12) & 0xf]; p[5] = hexd[(v >> 8) & 0xf];
    p[6] = hexd[(v >> 4) & 0xf];  p[7] = hexd[v & 0xf];
}

static uint32_t get_hex32_be(const char *p)
{
    uint32_t v = 0; int i;
    for (i = 0; i < 8; i++) { int h = hexval(p[i]); if (h < 0) break; v = (v << 4) | h; }
    return v;
}

/* --- stop-reply + continue ----------------------------------------------- */

static void send_stop(int fd, int sig) { char r[8]; sprintf(r, "S%02x", sig & 0xff); send_packet(fd, r); }

/* Poll the socket (non-blocking) for a pending Ctrl-C during a continue. */
static int check_interrupt(int fd)
{
    unsigned char c;
    ssize_t r = recv(fd, &c, 1, MSG_DONTWAIT);
    if (r == 1 && c == 0x03) return 1;
    return 0;
}

/* --- main serve loop ----------------------------------------------------- */

int gdb_serve(machine_t *m, int port)
{
    int ls, fd, one = 1;
    struct sockaddr_in sa;
    static char pkt[PKT_MAX];
    static char rsp[PKT_MAX];
    uint32_t bp[GDB_MAXBP];
    int nbp = 0;

    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { perror("socket"); return -1; }
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)port);
    if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) < 0) { perror("bind"); close(ls); return -1; }
    if (listen(ls, 1) < 0) { perror("listen"); close(ls); return -1; }
    fprintf(stderr, "[gdb] listening on 127.0.0.1:%d -- connect gdb with "
                    "'target remote :%d'\n", port, port);

    fd = accept(ls, NULL, NULL);
    close(ls);
    if (fd < 0) { perror("accept"); return -1; }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    fprintf(stderr, "[gdb] client connected\n");

    for (;;) {
        int n = recv_packet(fd, pkt, sizeof(pkt));
        if (n < 0) break;                 /* EOF */
        if (n == 0) { send_stop(fd, 2); continue; }   /* stray Ctrl-C -> SIGINT */

        char cmd = pkt[0];
        rsp[0] = '\0';

        switch (cmd) {
        case 'q':
            if (!strncmp(pkt, "qSupported", 10))
                snprintf(rsp, sizeof(rsp), "PacketSize=%x;hwbreak+", PKT_MAX);
            else if (!strcmp(pkt, "qC")) strcpy(rsp, "QC1");
            else if (!strcmp(pkt, "qAttached")) strcpy(rsp, "1");
            else if (!strcmp(pkt, "qfThreadInfo")) strcpy(rsp, "m1");
            else if (!strcmp(pkt, "qsThreadInfo")) strcpy(rsp, "l");
            else if (!strncmp(pkt, "qTStatus", 8)) strcpy(rsp, "T0");
            /* else: empty = unsupported */
            send_packet(fd, rsp);
            break;

        case '?':
            send_stop(fd, 5);
            break;

        case 'g': {                        /* read all registers */
            int i;
            for (i = 0; i < GDB_NREG; i++)
                put_hex32_be(rsp + i * 8, gdb_getreg(m, i));
            rsp[GDB_NREG * 8] = '\0';
            send_packet(fd, rsp);
            break;
        }

        case 'G': {                        /* write all registers */
            int i;
            for (i = 0; i < GDB_NREG && (int)strlen(pkt + 1) >= (i + 1) * 8; i++)
                gdb_setreg(m, i, get_hex32_be(pkt + 1 + i * 8));
            send_packet(fd, "OK");
            break;
        }

        case 'p': {                        /* read one register */
            int rn = (int)strtol(pkt + 1, NULL, 16);
            put_hex32_be(rsp, gdb_getreg(m, rn));
            rsp[8] = '\0';
            send_packet(fd, rsp);
            break;
        }

        case 'P': {                        /* write one register: P<n>=<val> */
            char *eq = strchr(pkt, '=');
            if (eq) { int rn = (int)strtol(pkt + 1, NULL, 16);
                      gdb_setreg(m, rn, get_hex32_be(eq + 1)); }
            send_packet(fd, "OK");
            break;
        }

        case 'm': {                        /* read memory: m<addr>,<len> */
            char *comma = strchr(pkt, ',');
            uint32_t addr = (uint32_t)strtoul(pkt + 1, NULL, 16);
            uint32_t len = comma ? (uint32_t)strtoul(comma + 1, NULL, 16) : 0;
            uint32_t i;
            if (len > PKT_MAX / 2) len = PKT_MAX / 2;
            for (i = 0; i < len; i++) {
                uint32_t b = machine_dbg_read(m, addr + i, 1);
                rsp[i * 2] = hexd[(b >> 4) & 0xf];
                rsp[i * 2 + 1] = hexd[b & 0xf];
            }
            rsp[len * 2] = '\0';
            send_packet(fd, rsp);
            break;
        }

        case 'M': {                        /* write memory: M<addr>,<len>:<hex> */
            char *comma = strchr(pkt, ',');
            char *colon = strchr(pkt, ':');
            uint32_t addr = (uint32_t)strtoul(pkt + 1, NULL, 16);
            uint32_t len = comma ? (uint32_t)strtoul(comma + 1, NULL, 16) : 0;
            uint32_t i;
            if (colon) {
                for (i = 0; i < len; i++) {
                    int hi = hexval(colon[1 + i * 2]), lo = hexval(colon[2 + i * 2]);
                    if (hi < 0 || lo < 0) break;
                    machine_dbg_write(m, addr + i, (uint32_t)((hi << 4) | lo), 1);
                }
            }
            send_packet(fd, "OK");
            break;
        }

        case 'Z':                          /* insert breakpoint: Z0,addr,kind */
        case 'z': {                        /* remove breakpoint */
            char *c1 = strchr(pkt, ',');
            uint32_t addr = c1 ? (uint32_t)strtoul(c1 + 1, NULL, 16) : 0;
            int i;
            if (cmd == 'Z') {
                int have = 0;
                for (i = 0; i < nbp; i++) if (bp[i] == addr) have = 1;
                if (!have && nbp < GDB_MAXBP) bp[nbp++] = addr;
                send_packet(fd, "OK");
            } else {
                for (i = 0; i < nbp; i++)
                    if (bp[i] == addr) { bp[i] = bp[--nbp]; break; }
                send_packet(fd, "OK");
            }
            break;
        }

        case 's': {                        /* single step [addr] */
            uint64_t ran = 0;
            if (pkt[1]) m->cpu.pc = (uint32_t)strtoul(pkt + 1, NULL, 16);
            int rc = machine_step_bp(m, bp, nbp, 1, &ran);
            send_stop(fd, rc < 0 ? 5 : 5);
            break;
        }

        case 'c': {                        /* continue [addr] */
            if (pkt[1]) m->cpu.pc = (uint32_t)strtoul(pkt + 1, NULL, 16);
            int sig = 5, stopped = 0;
            while (!stopped) {
                uint64_t ran = 0;
                int rc = machine_step_bp(m, bp, nbp, GDB_BATCH, &ran);
                if (rc == 1) { sig = 5; break; }            /* breakpoint */
                if (rc < 0)  { sig = 5; break; }            /* halted */
                if (check_interrupt(fd)) { sig = 2; break; }/* Ctrl-C */
                /* rc==0: batch exhausted, loop and keep running */
            }
            send_stop(fd, sig);
            break;
        }

        case 'H':  send_packet(fd, "OK"); break;   /* set thread -> ok */
        case '!':  send_packet(fd, "OK"); break;   /* extended mode */
        case 'D':  send_packet(fd, "OK"); goto done;   /* detach */
        case 'k':  goto done;                          /* kill */

        default:
            send_packet(fd, "");           /* unsupported -> empty */
            break;
        }
    }
done:
    fprintf(stderr, "[gdb] session ended (icount=%llu pc=0x%08x)\n",
            (unsigned long long)m->cpu.icount, m->cpu.pc);
    close(fd);
    return 0;
}
