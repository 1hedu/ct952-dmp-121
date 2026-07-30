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
/* Which bringup step failed, kept for the final on-screen summary: the per-attempt
 * lines scroll off a 7-row console, so the reason has to persist.
 * 1 idle line  2 port not enabled  3 GET_DESCRIPTOR  4 SET_ADDRESS
 * 5 GET_CONFIG  6 SET_CONFIGURATION  0 success */
static int g_failstep = 0;
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
    g_qh[1] = (uint32_t)g_dev_addr | g_eps | QH_DTC | QH_H | QH_MPS(g_isls ? 8 : 64)
              | (g_isls ? QH_C : 0u);
    g_qh[2] = QH_MULT1 | (g_isls ? (QH_HUBADDR(TT_HUB_ADDR) | QH_PORTNUM(1)) : 0u);
    g_qh[3] = 0;                            /* current qTD                    */
    g_qh[4] = pa(ts);                       /* overlay: next qTD -> SETUP     */
    g_qh[5] = QTD_T;                        /* overlay: alt next              */
    g_qh[6] = 0;                            /* overlay: token (idle)          */
    for (int i = 7; i < 12; i++) g_qh[i] = 0;

    EHCI_ASYNCLB = pa(g_qh);
    EHCI_USBCMD  = USBCMD_RS | USBCMD_ASE;

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
    td[0] = QTD_T;
    td[1] = QTD_T;
    td[2] = QTD_ACTIVE | QTD_PID_IN | QTD_CERR | QTD_BYTES(8) |
            (g_int_toggle ? QTD_DT : 0);
    td[3] = pa(g_data);
    td[4] = td[5] = td[6] = td[7] = 0;

    g_qh[0] = pa(g_qh) | (1u << 1);
    g_qh[1] = (uint32_t)g_dev_addr | (1u << 8) /* ep 1 */ |
              g_eps | QH_DTC | QH_H | QH_MPS(8);
    g_qh[2] = QH_MULT1 | (g_isls ? (QH_HUBADDR(TT_HUB_ADDR) | QH_PORTNUM(1) |
                                    QH_SMASK_C | QH_CMASK_C) : 0u);
    g_qh[3] = 0;
    g_qh[4] = pa(td);
    g_qh[5] = QTD_T;
    g_qh[6] = 0;
    for (int i = 7; i < 12; i++) g_qh[i] = 0;

    EHCI_ASYNCLB = pa(g_qh);
    EHCI_USBCMD  = USBCMD_RS | USBCMD_ASE;

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

/* Reset the controller + root-hub port, enumerate and configure the keyboard.
 * Returns 1 on success, 0 if no device / enumeration failed. Plain C so both
 * the Python module and the C REPL stdin path can call it. */
int usb_kbd_bringup(void) {
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
        mp_printf(&mp_plat_print, "usb HCCAP=%08x caplen=%02x op=%08x\n",
                  (unsigned)cap, (unsigned)caplen, (unsigned)g_op);
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
    EHCI_PORTSC0 = EHCI_PORTSC0 | PORTSC_PP;
    for (volatile int i = 0; i < 600000; i++) { }
    mp_printf(&mp_plat_print, "usb1 PORTSC=%08x\n", (unsigned)EHCI_PORTSC0);

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
    /* Clear the connect-change latch, then reset the port. */
    EHCI_PORTSC0 = (EHCI_PORTSC0 & ~PORTSC_PED) | PORTSC_CSC;
    EHCI_PORTSC0 = EHCI_PORTSC0 | PORTSC_PR;
    for (volatile int i = 0; i < 20000; i++) { }
    EHCI_PORTSC0 = EHCI_PORTSC0 & ~PORTSC_PR;   /* de-assert -> HS enable */
    for (volatile int i = 0; i < 20000; i++) { }
    mp_printf(&mp_plat_print, "usb2 PORTSC=%08x LS=%d\n", (unsigned)EHCI_PORTSC0,
              (int)((EHCI_PORTSC0 >> 10) & 3));
    if (!(EHCI_PORTSC0 & PORTSC_PED)) {
        mp_printf(&mp_plat_print, "usb FAIL: port not enabled after reset\n");
        g_failstep = 2; return 0;
    }

    /* Enumerate at address 0: read the 18-byte device descriptor. */
    if (ctrl_xfer(0x80, REQ_GET_DESCRIPTOR, (DESC_DEVICE << 8), 0, 18) < 18) {
        mp_printf(&mp_plat_print, "usb FAIL: GET_DESCRIPTOR (PORTSC=%08x)\n",
                  (unsigned)EHCI_PORTSC0);
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
