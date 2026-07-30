/* MicroPython "usb_kbd" module: a bare-metal USB HID boot-keyboard driver for
 * the CT952's EHCI host controller, driven from Python.
 *
 * This is a real (if minimal) EHCI driver: it resets the controller, resets
 * the root-hub port, then enumerates the attached device over the async
 * schedule with standard USB control transfers (GET_DESCRIPTOR, SET_ADDRESS,
 * SET_CONFIGURATION, HID SET_PROTOCOL), and finally polls the interrupt-IN
 * endpoint for 8-byte HID boot reports. It talks to ct952emu's EHCI model +
 * HID keyboard device (jupiter/emu/machine.c, §12.119).
 *
 * The controller executes Queue Heads / qTDs by DMA out of DRAM. On this SoC
 * the schedule structures are CPU-native big-endian, so the driver builds them
 * with ordinary 32-bit stores -- no byte swapping. Buffers live in .bss (DRAM).
 */
#include <stdint.h>
#include <string.h>
#include "py/runtime.h"
#include "py/obj.h"
#include "py/mphal.h"

/* ---- USB host registers -------------------------------------------------------
 * This is a ChipIdea/Freescale-style USB 2.0 OTG core, NOT a bare EHCI with a
 * 0x10-byte capability block. Proven from the firmware's own accesses:
 *   0xA0000184  read + "btst 1"   -> PORTSC1, bit 0 = CCS (device connected)
 *   0xA00001A4  read + "and 0x200"-> OTGSC, VBUS-valid check
 *   0xA0000164  |= 0x7f0000       -> TXFILLTUNING
 * Those are exactly OP+0x44, OP+0x64 and OP+0x24 for an operational base of
 * 0xA0000140, i.e. HCCAPBASE at 0xA0000100 with **CAPLENGTH = 0x40**.
 *
 * The previous code assumed CAPLENGTH = 0x10 and so had every operational
 * register 0x30 too low: it polled 0xA0000154 as PORTSC, which is really
 * PERIODICLISTBASE and reads 0 -- hence "no CCS (nothing connected)" on hardware
 * with a keyboard plugged in. Derive the base from CAPLENGTH now instead of
 * assuming it.
 *
 * Corollary: a ChipIdea core drives FULL and LOW speed devices itself, with no
 * companion controller and no hub transaction translator, so a low-speed keyboard
 * is fine once the right registers are used. (The earlier "EHCI cannot do
 * low-speed" reasoning is true of discrete EHCI but not of this core.) */
#define EHCI_CAP_BASE    0xA0000100u
#define EHCI_CAPLENGTH   (*(volatile uint32_t *)(EHCI_CAP_BASE + 0x00))
static uint32_t g_op = 0xA0000140u;      /* set from CAPLENGTH in bringup */
#define OPREG(o)         (*(volatile uint32_t *)(uintptr_t)(g_op + (o)))
#define EHCI_USBCMD      OPREG(0x00)
#define EHCI_USBSTS      OPREG(0x04)
#define EHCI_PERIODICLB  OPREG(0x14)
#define EHCI_ASYNCLB     OPREG(0x18)
#define EHCI_CONFIGFLAG  OPREG(0x40)
#define EHCI_PORTSC0     OPREG(0x44)
#define EHCI_OTGSC       OPREG(0x64)
#define EHCI_TTCTRL      OPREG(0x24)
#define EHCI_USBMODE     OPREG(0x68)
#define USBMODE_CM_HOST  0x00000003u     /* controller mode = host */

#define USBCMD_RS        0x00000001u   /* Run/Stop                */
#define USBCMD_HCRESET   0x00000002u   /* Host Controller Reset   */
#define USBCMD_ASE       0x00000020u   /* Async Schedule Enable   */

#define PORTSC_PP        0x00001000u   /* port power                          */
#define PORTSC_CCS       0x00000001u   /* Current Connect Status  */
#define PORTSC_CSC       0x00000002u   /* Connect Status Change   */
#define PORTSC_PED       0x00000004u   /* Port Enabled            */
#define PORTSC_PR        0x00000100u   /* Port Reset              */
#define PORTSC_PEC       0x00000008u   /* Port Enable Change (RW1C)           */
#define PORTSC_OCC       0x00000020u   /* Over-current Change (RW1C)          */
#define PORTSC_RW1C      (PORTSC_CSC | PORTSC_PEC | PORTSC_OCC)
/* Port Force Full Speed Connect. Found in the STOCK FIRMWARE's own EHCI stack, which
 * is a BSD-derived driver with Transdimension/ChipIdea quirks ("TDI EHCI OTG
 * Controller"). Its ehci_init prints "forcing host to connect as full speed" and then
 * does exactly: read PORTSC (= regbase+0x44, which independently confirms our operational
 * base), AND with 0xFFFFFFD5 to preserve everything except the write-1-to-clear change
 * bits, OR in bit 24, write back. It is gated on a board flag the firmware keeps at
 * 0x40040f0c. Setting it makes the port connect at full/low speed instead of chirping for
 * high speed, which on some PHYs is required for a low-speed device to work at all. */
#define PORTSC_PFSC      0x01000000u

/* Read-modify-write PORTSC the way the firmware does: never write 1 back to a change bit
 * that happens to be set, or the write silently acknowledges an event we have not handled.
 * The old code did plain `PORTSC |= x` read-modify-writes and clobbered them. */
static int g_pfsc = 0;               /* try the quirk on the second bringup attempt */
static void portsc_rmw(uint32_t set, uint32_t clr)
{
    uint32_t v = OPREG(0x44) & ~(PORTSC_RW1C | clr);
    OPREG(0x44) = v | set;
}

/* ---- qTD token bits --------------------------------------------------------- */
#define QTD_T            0x00000001u   /* terminate (invalid pointer)         */
#define QTD_ACTIVE       0x00000080u
#define QTD_PID_OUT      (0u << 8)
#define QTD_PID_IN       (1u << 8)
#define QTD_PID_SETUP    (2u << 8)
#define QTD_CERR         (3u << 10)    /* 3 error retries                     */
#define QTD_IOC          0x00008000u
#define QTD_DT           0x80000000u   /* data toggle                         */
#define QTD_BYTES(n)     (((uint32_t)(n) & 0x7FFF) << 16)

/* ---- QH endpoint-characteristics bits (word 1) ------------------------------ */
#define QH_EPS_FS        (0u << 12)    /* full-speed endpoint                 */
#define QH_EPS_LS        (1u << 12)    /* LOW-speed endpoint                  */
#define QH_EPS_HS        (2u << 12)    /* high-speed endpoint                 */
#define QH_C             (1u << 27)    /* control endpoint (set for LS/FS)    */
/* Endpoint speed actually detected on the port, filled in during bringup. A USB
 * keyboard is LOW-speed, so hardcoding QH_EPS_HS made every transfer target a
 * speed the device does not run at. */
static uint32_t g_eps = QH_EPS_HS;
static uint32_t g_isls = 0;
/* Endpoint-0 max packet size. Starts at the smallest legal value (8, which every
 * speed supports) and is replaced by bMaxPacketSize0 from the device descriptor once
 * we have read it -- rather than guessed from the port speed. */
static uint32_t g_mps0 = 8;
/* Which bringup step failed, kept for the final on-screen summary: the per-attempt
 * lines scroll off a 7-row console, so the reason has to persist.
 * 1 idle line  2 port not enabled  3 GET_DESCRIPTOR  4 SET_ADDRESS
 * 5 GET_CONFIG  6 SET_CONFIGURATION  0 success */
static int g_failstep = 0;
/* Controller state captured at the first failed transfer, so the reason survives the
 * scrolling console. Guessing has been exhausted; these bits state what happened:
 *   qTD token bit7 Active  (still set => the controller never executed the qTD)
 *             bit6 Halted, bit5 DataBufErr, bit4 Babble, bit3 XactErr,
 *             bit2 MissedUframe, bit1 SplitXstate, bits11:10 CERR
 *   USBSTS bit12 HCHalted, bit4 HostSysErr, bit3 FrameRollover, bit1 ErrInt */
static uint32_t g_dbg_setup, g_dbg_data, g_dbg_status, g_dbg_usbsts, g_dbg_frindex, g_dbg_qh3;
/* SETUP bytes read back out of DMA memory after a failed transfer, and the DATA-stage
 * status recorded for each split-routing variant tried. */
static uint8_t  g_dbg_sbytes[8];
static uint32_t g_dbg_nodev, g_dbg_portsc, g_dbg_bare_in, g_dbg_rx;
uint32_t usb_kbd_barein(void) { return g_dbg_bare_in; }
uint32_t usb_kbd_rx(void)     { return g_dbg_rx; }
uint32_t usb_kbd_nodev(void)  { return g_dbg_nodev; }
uint32_t usb_kbd_portsc(void) { return g_dbg_portsc; }
static uint32_t g_dbg_dv[8];   /* [0..2] per routing, [3] the zero-length probe */
uint32_t usb_kbd_dv(int i)       { return g_dbg_dv[i & 7]; }
uint32_t usb_kbd_setupbyte(int i){ return g_dbg_sbytes[i & 7]; }
uint32_t usb_kbd_dbg(int which)
{
    switch (which) {
    case 0: return g_dbg_setup;   case 1: return g_dbg_status;
    case 2: return g_dbg_usbsts;  case 3: return g_dbg_frindex;
    case 5: return g_dbg_data;
    default: return g_dbg_qh3;
    }
}
int usb_kbd_failstep(void) { return g_failstep; }
int usb_kbd_lastxfer(void);
#define QH_DTC           (1u << 14)    /* take data toggle from the qTD       */
#define QH_H             (1u << 15)    /* head of reclamation list            */
#define QH_MPS(n)        (((uint32_t)(n) & 0x7FF) << 16)
#define QH_MULT1         (1u << 30)    /* one transaction per uframe (word 2) */
/* Split-transaction fields in QH word 2, needed for a LOW/FULL-speed device reached
 * through a transaction translator. This core has an EMBEDDED TT, and the firmware
 * configures it by setting TTCTRL.TTHA (0xA0000164 |= 0x7f0000), i.e. hub address
 * 0x7F. A QH must carry that same hub address, plus the port number, or the
 * controller never routes the transfer through the TT -- which is exactly why the
 * first control transfer to the keyboard failed (FAILSTEP=3) while the port itself
 * was powered, connected and correctly identified as low-speed. */
#define QH_HUBADDR(a)    (((uint32_t)(a) & 0x7Fu) << 16)
#define QH_PORTNUM(p)    (((uint32_t)(p) & 0x7Fu) << 23)
#define TT_HUB_ADDR      0x7Fu          /* matches TTCTRL.TTHA set by the firmware */
/* Which split-transaction routing to use for a low-speed device on the ROOT port. The
 * firmware setting TTCTRL.TTHA does not prove that a QH must carry that hub address:
 * this core can drive a directly attached low/full-speed device either through its
 * embedded TT (split transactions) or natively at that speed. The choice is not
 * cosmetic -- if we issue splits when the port is not behind a TT, the TT itself can
 * answer STALL, which in the qTD status is indistinguishable from the device
 * stalling. Hardware reported exactly a bare Halted (STALL) with a spec-legal 60ms
 * reset, so try each routing and report which one the device answers. */
#define TT_MODE_PORT1    0
#define TT_MODE_PORT0    1
#define TT_MODE_NONE     2
static int g_tt_mode = TT_MODE_PORT1;
/* Whether to set the QH's C (control-endpoint) bit, also under test: it selects the
 * split-transaction control path for a low/full-speed endpoint. */
static int g_ctrl_c = 1;
#define QH_SMASK_C       0x00000001u    /* start-split in uframe 0 (periodic)       */
#define QH_CMASK_C       0x00001C00u    /* complete-split in uframes 2..4 (periodic)*/

/* Standard USB request codes. */
#define REQ_GET_DESCRIPTOR   0x06
#define REQ_SET_ADDRESS      0x05
#define REQ_SET_CONFIGURATION 0x09
#define HID_SET_PROTOCOL     0x0B
#define HID_SET_IDLE         0x0A
#define DESC_DEVICE          0x01
#define DESC_CONFIG          0x02

/* ---- DMA-visible structures in DRAM (.bss). 32-byte aligned per EHCI. -------
 * CACHE COHERENCY: the host controller reads and writes these by DMA straight out
 * of DRAM, while the CPU goes through its write-back D-cache. Written normally, the
 * descriptors sit dirty in cache and the controller DMAs stale garbage; worse,
 * qtd_wait() polls the qTD status word, so a cached read would never observe the
 * controller's completion write and every transfer would appear to fail. That is
 * consistent with FAILSTEP=3 persisting after the port itself came fully up.
 *
 * This SoC provides a D-cache BYPASS alias of DRAM, documented in the platform
 * header: PLAT_BYPASS_DCACHE_STARTADR = 0xC0000000 (ctkav_platform.h:683). So keep
 * the storage in .bss but touch it only through that alias, and hand the controller
 * the ordinary 0x4xxxxxxx physical address.
 *
 * The emulator cannot expose this class of bug: it has no D-cache, so cached and
 * uncached accesses are identical there. */
static volatile uint32_t g_qh_s[12]   __attribute__((aligned(32)));  /* one Queue Head */
static volatile uint32_t g_td_s[3][8] __attribute__((aligned(32)));  /* SETUP/DATA/STATUS */
static uint8_t           g_setup_s[8] __attribute__((aligned(8)));
static uint8_t           g_data_s[256]__attribute__((aligned(8)));

#define UNCACHED(p) ((uintptr_t)(((uintptr_t)(p) & 0x0FFFFFFFu) | 0xC0000000u))

/* uncached views, set up by dma_view_init() before any transfer */
static volatile uint32_t *g_qh    = g_qh_s;
static volatile uint32_t *g_td_p[3];
static uint8_t           *g_setup = g_setup_s;
static uint8_t           *g_data  = g_data_s;

static void dma_view_init(void)
{
    g_qh    = (volatile uint32_t *)UNCACHED(g_qh_s);
    g_td_p[0] = (volatile uint32_t *)UNCACHED(g_td_s[0]);
    g_td_p[1] = (volatile uint32_t *)UNCACHED(g_td_s[1]);
    g_td_p[2] = (volatile uint32_t *)UNCACHED(g_td_s[2]);
    g_setup = (uint8_t *)UNCACHED(g_setup_s);
    g_data  = (uint8_t *)UNCACHED(g_data_s);
}

static uint8_t g_dev_addr;     /* address we assigned to the keyboard */
static uint8_t g_int_toggle;   /* interrupt-IN data toggle            */
static uint8_t g_ready;        /* enumeration succeeded               */

/* Physical address for the controller. Accepts either a cached (0x4xxxxxxx) or a
 * bypass-alias (0xCxxxxxxx) pointer and always yields the DRAM physical address. */
static inline uint32_t pa(volatile void *p)
{
    return ((uint32_t)(uintptr_t)p & 0x0FFFFFFFu) | 0x40000000u;
}

/* Real-time delay, in milliseconds. The CPU clock is unknown, so spin-count delays
 * are guesswork; FRINDEX is a hardware timebase that ticks once per 125us microframe
 * (8 ticks per 1ms frame) whenever the controller is running, so derive the delay
 * from it. If FRINDEX is not advancing -- the controller is halted, or it counts
 * whole frames on this core -- the iteration guard still bounds the wait. The guard
 * is sized from measured hardware behaviour: ~2.7M spin iterations elapsed while
 * FRINDEX advanced 3352 counts, i.e. roughly 6.5 iterations per microsecond. */
static void hc_delay_ms(int ms)
{
    uint32_t f0 = OPREG(0x0C) & 0x3FFFu;
    long guard = (long)ms * 20000L;          /* ~3x the expected time, as a backstop */
    for (long i = 0; i < guard; i++) {
        uint32_t d = (OPREG(0x0C) - f0) & 0x3FFFu;
        if ((int)(d >> 3) >= ms) {
            return;
        }
    }
}

/* QH word 2 (endpoint capabilities). A high-speed device needs no TT fields at all;
 * a low-speed one needs them only if we are routing through the embedded TT. `periodic`
 * adds the split start/complete masks, which apply to interrupt endpoints only. */
static uint32_t qh_word2(int periodic)
{
    if (!g_isls || g_tt_mode == TT_MODE_NONE) {
        return QH_MULT1;
    }
    uint32_t w = QH_MULT1 | QH_HUBADDR(TT_HUB_ADDR) |
                 QH_PORTNUM(g_tt_mode == TT_MODE_PORT0 ? 0u : 1u);
    return periodic ? (w | QH_SMASK_C | QH_CMASK_C) : w;
}

/* Async schedule stop/start handshake.
 *
 * THIS IS WHY EVERY VARIANT REPORTED THE SAME THING. The old code left ASE set and
 * rewrote ASYNCLISTADDR and the Queue Head underneath a running schedule, which EHCI
 * forbids: the controller may hold a cached copy of the QH, including its overlay and
 * its Halted bit. Once the first transfer halted the queue, every later transfer
 * re-read that cached halted overlay and reported Halted immediately without ever
 * fetching the new QH -- so three different split routings and a zero-length probe all
 * came back 0x40, which is exactly what the hardware showed (d=40 40 40 z=40).
 *
 * The correct sequence is: clear ASE, wait for USBSTS.AS to follow it down, only then
 * publish the new list, then set ASE and wait for AS to come back up. */
#define USBSTS_AS        0x00008000u   /* Async Schedule Status (follows ASE) */

static void async_stop(void)
{
    EHCI_USBCMD = EHCI_USBCMD & ~USBCMD_ASE;
    for (int i = 0; i < 50 && (EHCI_USBSTS & USBSTS_AS); i++) {
        hc_delay_ms(1);
    }
    EHCI_USBSTS = EHCI_USBSTS;      /* clear the write-1-to-clear status bits */
}

static void async_start(uint32_t list)
{
    EHCI_ASYNCLB = list;
    EHCI_USBCMD  = EHCI_USBCMD | USBCMD_RS | USBCMD_ASE;
    for (int i = 0; i < 50 && !(EHCI_USBSTS & USBSTS_AS); i++) {
        hc_delay_ms(1);
    }
}

/* Spin until a qTD's Active bit clears, or a bounded budget elapses. Returns 1
 * if it retired, 0 on timeout (a NAK leaves the qTD Active). The controller
 * services the async ring on a background cadence, so we just poll memory. */
static int qtd_wait(volatile uint32_t *td, int budget) {
    for (int i = 0; i < budget; i++) {
        if (!(td[2] & QTD_ACTIVE)) {
            return 1;
        }
    }
    return 0;
}

/* Run a control transfer on endpoint 0. For IN transfers the data lands in
 * g_data; returns the number of bytes transferred (residual-corrected), or -1
 * on failure. */
static int ctrl_xfer(uint8_t bmRequestType, uint8_t bRequest,
                     uint16_t wValue, uint16_t wIndex, uint16_t wLength) {
    int dir_in = (bmRequestType & 0x80) != 0;
    /* Detach the schedule BEFORE touching the QH the controller may be caching. */
    async_stop();
    /* Build the 8-byte SETUP packet (USB fields are little-endian). */
    g_setup[0] = bmRequestType;
    g_setup[1] = bRequest;
    g_setup[2] = (uint8_t)wValue;  g_setup[3] = (uint8_t)(wValue >> 8);
    g_setup[4] = (uint8_t)wIndex;  g_setup[5] = (uint8_t)(wIndex >> 8);
    g_setup[6] = (uint8_t)wLength; g_setup[7] = (uint8_t)(wLength >> 8);

    volatile uint32_t *ts = g_td_p[0], *td = g_td_p[1], *tk = g_td_p[2];
    int have_data = (wLength > 0);

    /* SETUP stage qTD. */
    ts[0] = have_data ? pa(td) : pa(tk);   /* next qTD */
    ts[1] = QTD_T;                          /* alt next */
    ts[2] = QTD_ACTIVE | QTD_PID_SETUP | QTD_CERR | QTD_BYTES(8);  /* DT=0 */
    ts[3] = pa(g_setup);
    ts[4] = ts[5] = ts[6] = ts[7] = 0;

    if (have_data && dir_in) {
        for (int i = 0; i < 8 && i < (int)wLength; i++) {
            g_data[i] = 0;      /* so "no bytes arrived" cannot be read as stale data */
        }
    }
    if (have_data) {
        /* DATA stage qTD (DT starts at 1). */
        td[0] = pa(tk);
        td[1] = QTD_T;
        td[2] = QTD_ACTIVE | (dir_in ? QTD_PID_IN : QTD_PID_OUT) |
                QTD_CERR | QTD_BYTES(wLength) | QTD_DT;
        td[3] = pa(g_data);
        td[4] = td[5] = td[6] = td[7] = 0;
    }

    /* STATUS stage qTD: opposite direction, zero length, DT=1. */
    tk[0] = QTD_T;
    tk[1] = QTD_T;
    tk[2] = QTD_ACTIVE | (dir_in ? QTD_PID_OUT : QTD_PID_IN) |
            QTD_CERR | QTD_IOC | QTD_DT;
    tk[3] = 0;
    tk[4] = tk[5] = tk[6] = tk[7] = 0;

    /* Queue Head: single-element async ring (points at itself, H set). */
    g_qh[0] = pa(g_qh) | (1u << 1);        /* horizontal link, Typ=QH(01)   */
    /* control endpoint: speed from the port, and the C bit is required for a
     * low/full-speed control endpoint on this core */
    g_qh[1] = (uint32_t)g_dev_addr | g_eps | QH_DTC | QH_H | QH_MPS(g_mps0)
              | ((g_isls && g_ctrl_c) ? QH_C : 0u);
    g_qh[2] = qh_word2(0);
    g_qh[3] = 0;                            /* current qTD                    */
    g_qh[4] = pa(ts);                       /* overlay: next qTD -> SETUP     */
    g_qh[5] = QTD_T;                        /* overlay: alt next              */
    g_qh[6] = 0;                            /* overlay: token (idle)          */
    for (int i = 7; i < 12; i++) g_qh[i] = 0;

    async_start(pa(g_qh));

    /* Wait for the status stage to retire (whole transfer complete). */
    if (!qtd_wait(tk, 2000000)) {
        return -1;
    }
    if (have_data && dir_in) {
        uint32_t resid = (td[2] >> 16) & 0x7FFF;   /* bytes not transferred */
        int got = (int)wLength - (int)resid;
        return got < 0 ? 0 : got;
    }
    return 0;
}

/* Poll the interrupt-IN endpoint (0x81) once for an 8-byte HID boot report.
 * Copies the report into `out` and returns 1 if a report arrived, 0 if the
 * endpoint NAKed (no key change) within the budget. */
static int int_in_poll(uint8_t *out, int budget) {
    volatile uint32_t *td = g_td_p[0];
    async_stop();
    td[0] = QTD_T;
    td[1] = QTD_T;
    td[2] = QTD_ACTIVE | QTD_PID_IN | QTD_CERR | QTD_BYTES(8) |
            (g_int_toggle ? QTD_DT : 0);
    td[3] = pa(g_data);
    td[4] = td[5] = td[6] = td[7] = 0;

    g_qh[0] = pa(g_qh) | (1u << 1);
    g_qh[1] = (uint32_t)g_dev_addr | (1u << 8) /* ep 1 */ |
              g_eps | QH_DTC | QH_H | QH_MPS(8);
    g_qh[2] = qh_word2(1);
    g_qh[3] = 0;
    g_qh[4] = pa(td);
    g_qh[5] = QTD_T;
    g_qh[6] = 0;
    for (int i = 7; i < 12; i++) g_qh[i] = 0;

    async_start(pa(g_qh));

    if (!qtd_wait(td, budget)) {
        td[2] = 0;                 /* cancel the pending qTD (NAK / no key) */
        return 0;
    }
    memcpy(out, g_data, 8);
    g_int_toggle ^= 1;
    return 1;
}

/* ---- HID usage -> ASCII (US layout, unshifted + a few shifted) -------------- */
static char usage_to_ascii(uint8_t mod, uint8_t u) {
    int shift = (mod & 0x22) != 0;   /* left/right shift */
    if (u >= 0x04 && u <= 0x1D) {    /* a..z */
        char c = (char)('a' + (u - 0x04));
        return shift ? (char)(c - 32) : c;
    }
    if (u >= 0x1E && u <= 0x26) {    /* 1..9 */
        static const char *s = "!@#$%^&*(";
        return shift ? s[u - 0x1E] : (char)('1' + (u - 0x1E));
    }
    switch (u) {
    case 0x27: return shift ? ')' : '0';
    case 0x28: return '\n';   /* Enter     */
    case 0x2B: return '\t';   /* Tab       */
    case 0x2C: return ' ';    /* Space     */
    case 0x2D: return shift ? '_' : '-';
    case 0x2E: return shift ? '+' : '=';
    case 0x2F: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    default:   return 0;
    }
}

/* Is an IN transaction structurally possible at all?
 *
 * Hardware says every SETUP/OUT stage succeeds and every IN stage comes back bare
 * Halted with no bytes transferred, while a transfer to a nonexistent address fails
 * differently (XactErr, CERR=0) -- so packets do reach the wire and the controller can
 * tell silence from a stall. This probe issues a lone IN to endpoint 0 with no control
 * transfer pending. A device answers that with NAK, and a NAK is invisible in the qTD
 * status: the controller simply retries, so the qTD stays ACTIVE and this times out.
 *
 *   result 0x80 (still Active)  -> INs work at the bus level; NAKs are being collected,
 *                                  so the halts really are protocol responses.
 *   result 0x40 (Halted)        -> every IN halts regardless of what the device says,
 *                                  i.e. IN completion is structurally broken (the
 *                                  complete-split half of a split transaction).
 */
static uint32_t probe_bare_in(void)
{
    volatile uint32_t *td = g_td_p[0];
    async_stop();
    td[0] = QTD_T;
    td[1] = QTD_T;
    td[2] = QTD_ACTIVE | QTD_PID_IN | QTD_CERR | QTD_BYTES(8);
    td[3] = pa(g_data);
    td[4] = td[5] = td[6] = td[7] = 0;

    g_qh[0] = pa(g_qh) | (1u << 1);
    g_qh[1] = g_eps | QH_DTC | QH_H | QH_MPS(8) |
              ((g_isls && g_ctrl_c) ? QH_C : 0u);
    g_qh[2] = qh_word2(0);
    g_qh[3] = 0;
    g_qh[4] = pa(td);
    g_qh[5] = QTD_T;
    g_qh[6] = 0;
    for (int i = 7; i < 12; i++) g_qh[i] = 0;

    async_start(pa(g_qh));
    qtd_wait(td, 400000);
    return td[2];
}

/* Reset the controller + root-hub port, enumerate and configure the keyboard.
 * Returns 1 on success, 0 if no device / enumeration failed. Plain C so both
 * the Python module and the C REPL stdin path can call it. */
int usb_kbd_bringup(void) {
    static int attempt_no = 0;
    /* Alternate the force-full-speed quirk between attempts so both are covered without
     * another flash cycle; PORTSC bit 24 in the reported P= says which one was in use. */
    g_pfsc = (attempt_no++ & 1);
    g_ready = 0; g_dev_addr = 0; g_int_toggle = 0; g_failstep = 0;
    dma_view_init();   /* uncached views before any DMA structure is touched */

    /* Controller reset, then take ownership of all ports and run. */
    /* Derive the operational base from the capability register rather than
     * assuming it (that assumption is what broke this on real silicon). */
    {
        uint32_t cap = EHCI_CAPLENGTH;
        uint32_t caplen = cap & 0xFFu;
        if (caplen >= 0x10u && caplen <= 0x80u)
            g_op = EHCI_CAP_BASE + caplen;
        (void)cap;
    }

    EHCI_USBCMD = USBCMD_HCRESET;
    for (volatile int i = 0; i < 10000; i++) { }
    /* A ChipIdea core comes out of reset in device mode; it must be told to be a
     * HOST before the port will report a connection. The firmware does this via
     * USB_HCInit, which we are replacing, so we must do it ourselves. */
    EHCI_USBMODE = (EHCI_USBMODE & ~0x00000003u) | USBMODE_CM_HOST;
    /* Point the embedded TT at hub address 0x7F, exactly as the firmware does
     * (TTCTRL |= 0x7f0000). Transfers whose QH carries this hub address are routed
     * through the TT, which is how a directly attached low/full-speed device is
     * reached on this core. */
    EHCI_TTCTRL = (EHCI_TTCTRL & ~0x007F0000u) | ((uint32_t)TT_HUB_ADDR << 16);
    EHCI_CONFIGFLAG = 1;                 /* route ports to the host controller */
    EHCI_USBCMD = USBCMD_RS;
    for (volatile int i = 0; i < 50000; i++) { }   /* let the port sample D+/D- */

    /* Staged diagnostics. On hardware this failed silently, so report WHICH step
     * fails and the PORTSC value, rather than inferring it. PORTSC line status
     * (bits 11:10) is the decisive field for a keyboard: 01 = LOW-SPEED device.
     * EHCI alone cannot talk to a low- or full-speed device -- it needs either a
     * companion (OHCI/UHCI) or a high-speed hub's transaction translator -- and
     * virtually every USB keyboard, including a Gearhead KB1500U, is low-speed. */
    /* PORT POWER. Hardware showed PORTSC=1C000400: line status 01 (a LOW-SPEED
     * device is on the wire) but PP=0 and CCS=0 -- the port was never powered, so
     * the connect could not latch. Set PP and give VBUS time to come up. */
    portsc_rmw(PORTSC_PP | (g_pfsc ? PORTSC_PFSC : 0u), 0);
    /* USB 2.0 timing after applying VBUS: a port must settle for >=100ms before a
     * connect means anything, and the connection must then be debounced for 100ms
     * (TATTDB) before reset. A cheap HID also needs that long just to boot its own
     * microcontroller. The old 600000-iteration wait was ~90ms in total, i.e. we were
     * resetting the port while the keyboard was still powering up. */
    hc_delay_ms(150);
    for (int i = 0; i < 100 && !(EHCI_PORTSC0 & PORTSC_CCS); i++) {
        hc_delay_ms(10);            /* up to 1s for the connect to appear */
    }
    hc_delay_ms(120);               /* attach debounce */


    /* Take the device speed from the line-status field rather than assuming
     * high-speed: 01 = low-speed, 10 = full-speed. */
    {
        uint32_t ls = (EHCI_PORTSC0 >> 10) & 3u;
        g_isls = (ls == 1u);
        g_eps  = g_isls ? QH_EPS_LS : QH_EPS_HS;
    }
    if (!(EHCI_PORTSC0 & PORTSC_CCS)) {
        /* A low-speed device may show a valid line state before CCS latches; only
         * give up if the wire is idle too. */
        if (((EHCI_PORTSC0 >> 10) & 3u) == 0u) {
            mp_printf(&mp_plat_print, "usb FAIL: no CCS, idle line\n");
            g_failstep = 1; return 0;
        }
        mp_printf(&mp_plat_print, "usb: no CCS but LS=%d, continuing\n",
                  (int)((EHCI_PORTSC0 >> 10) & 3u));
    }
    /* Clear the connect-change latch, then reset the port.
     *
     * TIMING IS NORMATIVE HERE. USB 2.0 requires the reset (SE0) to be driven for at
     * least 10ms, and EHCI tells root-hub software to hold it ~50ms; the device then
     * needs up to 10ms of recovery (TRSTRCY) before it will answer a SETUP. The
     * previous code held PR for 20000 spin iterations -- about 3ms by the measured
     * ~6.5 iterations/us -- and gave 3ms of recovery. A short reset leaves the device
     * in an indeterminate state rather than the Default state, which is consistent
     * with what the hardware reported: the SETUP packet is ACKed (s=00) but the
     * device STALLs the data stage (d=40) instead of returning its descriptor. */
    /* Clearing the connect-change latch is the one place a 1 SHOULD be written. */
    EHCI_PORTSC0 = (EHCI_PORTSC0 & ~(PORTSC_PED | PORTSC_PEC | PORTSC_OCC)) | PORTSC_CSC;
    portsc_rmw(PORTSC_PR, PORTSC_PED);   /* PE must be written 0 alongside PR */
    hc_delay_ms(60);
    portsc_rmw(0, PORTSC_PR);                   /* de-assert -> port enable */
    /* The controller finishes the reset itself and clears PR when done. */
    for (int i = 0; i < 40 && (EHCI_PORTSC0 & PORTSC_PR); i++) {
        hc_delay_ms(1);
    }
    hc_delay_ms(20);                            /* TRSTRCY recovery */

    if (!(EHCI_PORTSC0 & PORTSC_PED)) {
        mp_printf(&mp_plat_print, "usb FAIL: port not enabled after reset\n");
        g_failstep = 2; return 0;
    }

    /* Enumerate at address 0. Ask for only the first 8 bytes first: that is one
     * max-packet at any speed, needs no assumption about bMaxPacketSize0, and is what
     * every real host stack does. The first control transfer after a port reset is
     * also allowed to fail, so retry a few times before giving up. */
    g_mps0 = 8;
    int got = 0;
    /* Try each split-transaction routing in turn. The qTD status cannot distinguish a
     * STALL from the device from a STALL synthesised by a misused embedded TT, so let
     * the device settle it: whichever routing produces a descriptor is the right one,
     * and the DATA-stage status of each is recorded for the on-screen summary. */
    /* Sweep the split-transaction configuration. The three fields that decide whether
     * a low-speed control endpoint is reached through the embedded TT are the QH's hub
     * address, its endpoint speed, and its C (control) bit; the qTD status cannot tell a
     * device STALL from a TT that was handed a transaction it will not translate, so let
     * the device decide which combination is right. Six combinations, DATA-stage status
     * of each recorded, first one that returns a descriptor wins. */
    static const struct { int tt; uint32_t eps; int c; } combos[6] = {
        { TT_MODE_PORT1, QH_EPS_LS, 1 },   /* TT, low-speed endpoint, control bit  */
        { TT_MODE_PORT1, QH_EPS_LS, 0 },   /* TT, no control bit                   */
        { TT_MODE_PORT1, QH_EPS_FS, 1 },   /* TT, endpoint declared full-speed     */
        { TT_MODE_NONE,  QH_EPS_LS, 1 },   /* no TT fields at all                  */
        { TT_MODE_NONE,  QH_EPS_LS, 0 },
        { TT_MODE_NONE,  QH_EPS_FS, 1 },
    };
    uint32_t eps_detected = g_eps;
    for (int ci = 0; ci < 6; ci++) {
        g_tt_mode = combos[ci].tt;
        g_ctrl_c  = combos[ci].c;
        g_eps     = g_isls ? combos[ci].eps : eps_detected;
        got = ctrl_xfer(0x80, REQ_GET_DESCRIPTOR, (DESC_DEVICE << 8), 0, 8);
        g_dbg_dv[ci] = g_td_p[1][2];
        if (got >= 8) {
            break;
        }
        g_dbg_setup   = g_td_p[0][2];      /* SETUP qTD token   */
        g_dbg_data    = g_td_p[1][2];      /* DATA  qTD token   */
        g_dbg_status  = g_td_p[2][2];      /* STATUS qTD token  */
        g_dbg_usbsts  = EHCI_USBSTS;
        g_dbg_frindex = OPREG(0x0C);       /* FRINDEX: is the controller running? */
        g_dbg_qh3     = g_qh[6];           /* QH overlay token   */
        /* What the controller actually fetched as the SETUP packet, and whatever landed
         * in the IN buffer. The buffer is cleared before each IN, so a non-zero value
         * here means bytes really did arrive before the halt. */
        for (int b = 0; b < 8; b++) {
            g_dbg_sbytes[b] = g_setup[b];
        }
        g_dbg_rx = ((uint32_t)g_data[0] << 24) | ((uint32_t)g_data[1] << 16) |
                   ((uint32_t)g_data[2] << 8)  | (uint32_t)g_data[3];
        hc_delay_ms(10);
    }
    if (got < 8) {
        g_tt_mode = TT_MODE_PORT1;
        g_ctrl_c  = 1;
        g_eps     = eps_detected;
    }
    if (got < 8) {
        /* Last discriminator before giving up: does the device reject EVERYTHING, or
         * only our IN data stage? SET_ADDRESS(0) is a request the device already
         * satisfies and it has no data stage at all, so a clean status stage here says
         * the device is in Default state and answering us, and that the failure is
         * specific to the data phase (toggle / packet size / split completion). A
         * halted status stage says the device rejects us outright, i.e. it is not in
         * Default state and the reset still is not taking. */
        g_tt_mode = TT_MODE_PORT1;
        ctrl_xfer(0x00, REQ_SET_ADDRESS, 0, 0, 0);
        g_dbg_dv[7] = g_td_p[2][2];   /* slots 0..5 hold the combination sweep */
        /* CONTROL EXPERIMENT. Address 7 cannot exist -- no address has been assigned
         * yet, so nothing on the wire will answer. A working bus MUST fail differently
         * here: three attempts, CERR counted down to 0, XactErr (bit 3) set, i.e. 0x48
         * or 0x68. If this returns the SAME bare Halted 0x40 as every real request,
         * then 0x40 is not the device stalling us -- the controller is halting the
         * queue by itself and every protocol theory is void. This is the one reading
         * that separates "the keyboard refuses us" from "we never reached the wire". */
        g_dev_addr = 7;
        ctrl_xfer(0x80, REQ_GET_DESCRIPTOR, (DESC_DEVICE << 8), 0, 8);
        g_dbg_nodev = g_td_p[0][2];   /* SETUP token: no device should even ACK it */
        g_dev_addr = 0;
        g_dbg_bare_in = probe_bare_in();
        g_dbg_portsc = EHCI_PORTSC0;  /* is the port still connected+enabled by now? */
        g_failstep = 3; return 0;
    }
    /* Adopt the device's real endpoint-0 packet size for every later transfer. */
    if (g_data[7] == 8 || g_data[7] == 16 || g_data[7] == 32 || g_data[7] == 64) {
        g_mps0 = g_data[7];
    }
    /* Now the full 18-byte descriptor. */
    if (ctrl_xfer(0x80, REQ_GET_DESCRIPTOR, (DESC_DEVICE << 8), 0, 18) < 18) {
        g_failstep = 3; return 0;
    }
    mp_printf(&mp_plat_print,
        "usb_kbd: device VID=%04x PID=%04x class=%d MPS0=%d\n",
        g_data[8] | (g_data[9] << 8), g_data[10] | (g_data[11] << 8),
        g_data[4], g_data[7]);

    /* Assign address 1 and adopt it. */
    if (ctrl_xfer(0x00, REQ_SET_ADDRESS, 1, 0, 0) < 0) {
        g_failstep = 4; return 0;
    }
    g_dev_addr = 1;
    hc_delay_ms(5);   /* TSETADDR: the device needs 2ms before it answers on addr 1 */

    /* Read the configuration descriptor set (config+iface+HID+endpoint). */
    if (ctrl_xfer(0x80, REQ_GET_DESCRIPTOR, (DESC_CONFIG << 8), 0, 34) < 9) {
        g_failstep = 5; return 0;
    }
    /* Select the (only) configuration. */
    if (ctrl_xfer(0x00, REQ_SET_CONFIGURATION, g_data[5], 0, 0) < 0) {
        g_failstep = 6; return 0;
    }
    /* HID: boot protocol + infinite idle (report only on change). */
    ctrl_xfer(0x21, HID_SET_PROTOCOL, 0 /* boot */, 0, 0);
    ctrl_xfer(0x21, HID_SET_IDLE, 0, 0, 0);

    g_ready = 1;
    mp_printf(&mp_plat_print, "usb_kbd: configured, polling ep 0x81\n");
    return 1;
}

/* Non-blocking single-key read for the REPL's stdin. One short interrupt-IN
 * poll: returns the ASCII code of a key-down with a printable usage, or -1 for
 * NAK / key-up / non-printable / no keyboard. */
int usb_kbd_c_getchar(void) {
    if (!g_ready) {
        return -1;
    }
    uint8_t rep[8];
    if (int_in_poll(rep, 4000) && rep[2] != 0) {
        char c = usage_to_ascii(rep[0], rep[2]);
        if (c) {
            return (unsigned char)c;
        }
    }
    return -1;
}

/* ============================ Python surface ================================= */

/* usb_kbd.init() -- enumerate and configure the keyboard; returns True/False. */
static mp_obj_t usb_kbd_init(void) {
    return usb_kbd_bringup() ? mp_const_true : mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_0(usb_kbd_init_obj, usb_kbd_init);

/* usb_kbd.poll() -- one interrupt-IN poll. Returns the 8-byte HID boot report
 * as bytes if a key event arrived, else None. */
static mp_obj_t usb_kbd_poll(void) {
    if (!g_ready) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("usb_kbd not initialised"));
    }
    uint8_t rep[8];
    if (int_in_poll(rep, 200000)) {
        return mp_obj_new_bytes(rep, 8);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(usb_kbd_poll_obj, usb_kbd_poll);

/* usb_kbd.getchar() -- block-ish poll until a key-down report with a printable
 * usage arrives; return it as a 1-char str. Returns None after ~budget empty
 * polls so a caller loop can stay responsive. */
static mp_obj_t usb_kbd_getchar(void) {
    if (!g_ready) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("usb_kbd not initialised"));
    }
    uint8_t rep[8];
    for (int tries = 0; tries < 48; tries++) {
        if (int_in_poll(rep, 20000)) {
            if (rep[2] != 0) {                       /* key-down (first key) */
                char c = usage_to_ascii(rep[0], rep[2]);
                if (c) {
                    char s[1] = { c };
                    return mp_obj_new_str(s, 1);
                }
            }
            /* key-up (all zero) or a non-printable key: keep polling */
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(usb_kbd_getchar_obj, usb_kbd_getchar);

static const mp_rom_map_elem_t usb_kbd_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_usb_kbd) },
    { MP_ROM_QSTR(MP_QSTR_init),    MP_ROM_PTR(&usb_kbd_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_poll),    MP_ROM_PTR(&usb_kbd_poll_obj) },
    { MP_ROM_QSTR(MP_QSTR_getchar), MP_ROM_PTR(&usb_kbd_getchar_obj) },
};
static MP_DEFINE_CONST_DICT(usb_kbd_module_globals, usb_kbd_module_globals_table);

const mp_obj_module_t usb_kbd_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&usb_kbd_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_usb_kbd, usb_kbd_user_cmodule);
