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
/* TTCTRL is at +0x1C and TXFILLTUNING at +0x24 in this (i.MX / ChipIdea) layout, and
 * TTCTRL's hub-address field TTHA lives in bits 30:24 -- NOT bits 22:16 like the QH's
 * hub-address field. This was wrong for several builds: "TTCTRL" was defined as +0x24, so
 * every TTHA=0x7F write actually went into TXFILLTUNING (the TX FIFO threshold) and the
 * embedded TT's hub address stayed 0, while our queue heads addressed split transactions to
 * hub 0x7F. Nothing answers at a hub address that was never assigned.
 *
 * That mismatch has exactly the observed shape: a SETUP or OUT carries its payload in the
 * start-split and completes, but an IN needs the TT to buffer the device's response for a
 * later complete-split, so it never returns a byte. The file header above had the correct
 * annotation for +0x24 all along; the define below it contradicted the comment. */
#define EHCI_TTCTRL      OPREG(0x1C)
#define EHCI_TXFILLTUNE  OPREG(0x24)
#define TTCTRL_TTHA(a)   (((uint32_t)(a) & 0x7Fu) << 24)
#define EHCI_USBMODE     OPREG(0x68)
#define USBMODE_CM_HOST  0x00000003u     /* controller mode = host */
#define USBMODE_SDIS     0x00000010u     /* Stream Disable Mode (bit 4)         */

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

/* Read-modify-write PORTSC the way the firmware does: never write 1 back to a change bit
 * that happens to be set, or the write silently acknowledges an event we have not handled.
 * The old code did plain `PORTSC |= x` read-modify-writes and clobbered them. */
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
/* Which bringup step failed, kept for a one-line failure message.
 * 1 idle line  2 port not enabled  3 GET_DESCRIPTOR  4 SET_ADDRESS
 * 5 GET_CONFIG  6 SET_CONFIGURATION  0 success */
static int g_failstep = 0;
int usb_kbd_failstep(void) { return g_failstep; }
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
/* NAK-count reload, qh_endp bits 31:28. The firmware's QH builder sets this to 8 for
 * every endpoint (0xb17f8 loads 0x80000000 as the base of qh_endp; the control case ORs
 * in the C bit to make 0x88000000). It bounds how many NAKs the controller absorbs before
 * moving on, and a keyboard NAKs constantly, so leaving it 0 was another difference. */
#define QH_RL8           0x80000000u
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

/* QH word 2 (endpoint capabilities), built to match the STOCK FIRMWARE's own EHCI QH
 * builder (0xb16e8..0xb183c), which is the ground truth for this core + this board.
 *
 * The firmware reads device->myhsport (the high-speed hub the device sits behind). For a
 * device attached directly to the ROOT PORT -- which our keyboard is -- that pointer is
 * NULL, and the builder then sets **HubAddr = 0 and PortNum = 0** (0xb1710 -> 0xb1724:
 * `clr %l6; clr %l5`). It does NOT use TTCTRL.TTHA (0x7F) as the QH hub address; that was
 * the mistake behind every IN halting. It also sets **CMASK = 0x08** unconditionally --
 * even on a control endpoint (`or %o0, 0x800`, and 0x800 lands in bits 15:8 as 0x08) --
 * and adds **SMASK = 0x02** only for an interrupt endpoint (`or %o0, 0x802`). On this
 * embedded-TT core the complete-split mask is what makes the controller issue the CSPLIT
 * that pulls IN data back; with CMASK = 0 the IN data stage never completes, which is
 * exactly the "SETUP transmits, IN halts with 0 bytes" seen on hardware. */
#define QH_CMASK_FW      0x00000800u   /* CMASK = 0x08 (complete-split at uframe 3) */
#define QH_SMASK_FW      0x00000002u   /* SMASK = 0x02 (interrupt start-split)       */
static uint32_t qh_word2(int periodic)
{
    /* HubAddr = 0, PortNum = 0 for a root-port device, exactly as the firmware does. */
    return QH_MULT1 | QH_CMASK_FW | (periodic ? QH_SMASK_FW : 0u);
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

/* Wait up to `ms` REAL milliseconds (measured on the FRINDEX timebase) for a qTD to
 * retire. This matters for the interrupt endpoint: the controller only services the
 * schedule about once per 1ms frame, so a spin-count budget of a few thousand
 * iterations (~hundreds of us) expires before the transaction is even attempted, and
 * the poll cancels a report the device was about to deliver. That is what made the
 * keyboard drop most keystrokes ("press three, get one"). Returns 1 if it retired. */
static int qtd_wait_ms(volatile uint32_t *td, int ms) {
    uint32_t f0 = OPREG(0x0C) & 0x3FFFu;
    long guard = (long)ms * 40000L;   /* backstop if FRINDEX is not advancing */
    for (long i = 0; i < guard; i++) {
        if (!(td[2] & QTD_ACTIVE)) {
            return 1;
        }
        uint32_t d = (OPREG(0x0C) - f0) & 0x3FFFu;
        if ((int)(d >> 3) >= ms) {
            return 0;
        }
    }
    return 0;
}

/* Run a control transfer on endpoint 0. For IN transfers the data lands in
 * g_data; returns the number of bytes transferred (residual-corrected), or -1
 * on failure. */
/* Reverse the bytes within each aligned 32-bit word of a buffer.
 *
 * WHY: the EHCI queue heads and qTDs work when written with ordinary big-endian CPU
 * stores, which proves the SoC's DMA byte-swaps 32-bit words in hardware to feed the
 * little-endian controller (native BE store + hardware swap = correct value to the LE
 * core; that is the only way the buffer pointers and links resolve). But that same
 * hardware swap also hits the DATA buffers, so the 8 SETUP bytes we lay down in USB
 * order arrive on the wire word-swapped -- garbage. A device ACKs the SETUP packet
 * regardless (its CRC is valid over whatever bytes) and then STALLs the data stage
 * because the request is unparseable. That is an exact match for what hardware showed:
 * SETUP transmits and is ACKed, every IN halts with a decoded STALL, on a low-speed
 * keyboard AND a high-speed stick alike -- and the emulator, a functional model with no
 * DMA swap, never reproduced it. Pre-swapping the buffer cancels the hardware swap so
 * the wire sees the intended byte order. */
static void bswap32_buf(uint8_t *p, int nbytes) {
    for (int i = 0; i + 4 <= nbytes; i += 4) {
        uint8_t a = p[i], b = p[i + 1];
        p[i] = p[i + 3]; p[i + 1] = p[i + 2];
        p[i + 2] = b;    p[i + 3] = a;
    }
}

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
    /* Compensate for the SoC's hardware DMA word-swap (see bswap32_buf). */
    bswap32_buf(g_setup, 8);

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
    g_qh[1] = (uint32_t)g_dev_addr | g_eps | QH_DTC | QH_H | QH_RL8 | QH_MPS(g_mps0)
              | (g_isls ? QH_C : 0u);   /* C bit: non-high-speed control endpoint */
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
        if (got < 0) got = 0;
        /* Received IN data was word-swapped by the same hardware DMA swap; undo it so the
         * descriptor parses in USB byte order. Round up to whole words. */
        bswap32_buf(g_data, (got + 3) & ~3);
        return got;
    }
    return 0;
}

/* Poll the interrupt-IN endpoint (0x81) once for an 8-byte HID boot report, waiting up
 * to `ms` real milliseconds. Copies the report into `out` and returns 1 if a report
 * arrived, 0 if the endpoint NAKed (no key change) within the window. */
static int int_in_poll(uint8_t *out, int ms) {
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
              g_eps | QH_DTC | QH_H | QH_RL8 | QH_MPS(8);   /* interrupt IN: no C bit */
    g_qh[2] = qh_word2(1);
    g_qh[3] = 0;
    g_qh[4] = pa(td);
    g_qh[5] = QTD_T;
    g_qh[6] = 0;
    for (int i = 7; i < 12; i++) g_qh[i] = 0;

    async_start(pa(g_qh));

    /* Close the cancel race: qtd_wait_ms may time out in the same microsecond the
     * controller completes the transfer. Cancelling then (td[2]=0) would drop a report
     * the device DID send AND leave our data toggle one step behind the device's, so
     * every later report reads with the wrong toggle -- the source of occasional garbled
     * input (phantom shift -> capitals/symbols). So only treat it as a NAK if the qTD is
     * genuinely still Active; otherwise it really completed, so consume it. */
    if (!qtd_wait_ms(td, ms)) {
        if (td[2] & QTD_ACTIVE) {
            td[2] = 0;             /* genuine NAK / no key change */
            return 0;
        }
    }
    bswap32_buf(g_data, 8);   /* undo the hardware DMA word-swap on the HID report */
    memcpy(out, g_data, 8);
    g_int_toggle ^= 1;
    return 1;
}

/* Keyboard LED / lock state, bit0 Num, bit1 Caps, bit2 Scroll (matches the HID LED
 * report and the SET_REPORT payload in keyboard_spec_MI00.md §4). */
static uint8_t g_leds;
void usb_kbd_set_leds(uint8_t leds);   /* fwd: control-OUT SET_REPORT, defined below */

/* ---- HID usage -> ASCII (US layout) ----------------------------------------- */
static char usage_to_ascii(uint8_t mod, uint8_t u) {
    int shift = (mod & 0x22) != 0;   /* left/right shift */
    int ctrl  = (mod & 0x11) != 0;   /* left/right ctrl  */
    int caps  = (g_leds & 0x02) != 0;
    if (u >= 0x04 && u <= 0x1D) {    /* a..z */
        if (ctrl) {
            return (char)(u - 0x04 + 1);  /* Ctrl-A..Ctrl-Z -> 0x01..0x1A */
        }
        char c = (char)('a' + (u - 0x04));
        return (shift ^ caps) ? (char)(c - 32) : c;  /* caps lock inverts letter case */
    }
    if (u >= 0x1E && u <= 0x26) {    /* 1..9 */
        static const char *s = "!@#$%^&*(";
        return shift ? s[u - 0x1E] : (char)('1' + (u - 0x1E));
    }
    switch (u) {
    case 0x27: return shift ? ')' : '0';
    case 0x28: return '\r';   /* Enter -> CR (readline treats CR/LF as submit) */
    case 0x29: return 0x1B;   /* Escape    */
    case 0x2A: return 0x08;   /* Backspace -> BS (readline deletes on 8 and 127) */
    case 0x2B: return '\t';   /* Tab       */
    case 0x2C: return ' ';    /* Space     */
    case 0x2D: return shift ? '_' : '-';
    case 0x2E: return shift ? '+' : '=';
    case 0x2F: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    default:   return 0;
    }
}

/* Press/release edge detection for the boot keyboard.
 *
 * The 8-byte report holds up to six key slots as an UNORDERED SET, and the device sends
 * a report on every state change (SET_IDLE(0)). Emitting a character whenever slot 1 is
 * non-zero double-types: when a second key is added or lifted, slot 1 can still hold the
 * first key, so it re-fires. The Holtek spec (keyboard_spec_MI00.md §3) says to compare
 * each report against the previous one and emit only NEWLY pressed usages. A small queue
 * buffers those so the one-char-at-a-time REPL stdin path loses nothing. */
static uint8_t g_prev_keys[6];        /* usages down in the previous report */
static char    g_keyq[16];
static int     g_keyq_head, g_keyq_tail;

static void keyq_push(char c) {
    int n = (g_keyq_tail + 1) & 15;
    if (n != g_keyq_head) { g_keyq[g_keyq_tail] = c; g_keyq_tail = n; }
}
static int keyq_pop(void) {
    if (g_keyq_head == g_keyq_tail) return -1;
    char c = g_keyq[g_keyq_head];
    g_keyq_head = (g_keyq_head + 1) & 15;
    return (unsigned char)c;
}
static int key_was_down(uint8_t k) {
    for (int i = 0; i < 6; i++) if (g_prev_keys[i] == k) return 1;
    return 0;
}
/* Queue everything one newly-pressed usage produces. Lock keys toggle their LED and
 * emit nothing; navigation keys emit the VT100 escape sequences MicroPython's readline
 * understands (arrows = history / cursor move, Home/End/Delete); everything else emits
 * its ASCII byte. */
static void kbd_emit(uint8_t mod, uint8_t u) {
    switch (u) {
    case 0x39: g_leds ^= 0x02; usb_kbd_set_leds(g_leds); return;   /* Caps Lock   */
    case 0x53: g_leds ^= 0x01; usb_kbd_set_leds(g_leds); return;   /* Num Lock    */
    case 0x47: g_leds ^= 0x04; usb_kbd_set_leds(g_leds); return;   /* Scroll Lock */
    }
    const char *seq = 0;
    switch (u) {
    case 0x52: seq = "\x1b[A"; break;   /* Up    -> history back    */
    case 0x51: seq = "\x1b[B"; break;   /* Down  -> history forward */
    case 0x4F: seq = "\x1b[C"; break;   /* Right -> cursor right    */
    case 0x50: seq = "\x1b[D"; break;   /* Left  -> cursor left     */
    case 0x4A: seq = "\x1b[H"; break;   /* Home                     */
    case 0x4D: seq = "\x1b[F"; break;   /* End                      */
    case 0x4C: seq = "\x1b[3~"; break;  /* Delete Forward           */
    }
    if (seq) { while (*seq) keyq_push(*seq++); return; }
    char c = usage_to_ascii(mod, u);
    if (c) keyq_push(c);
}
/* Turn one 8-byte report into queued input for the keys that are newly down. */
static void process_report(const uint8_t *rep) {
    const uint8_t *cur = rep + 2;     /* the six key slots */
    /* ErrorRollOver: all six slots 0x01 -> discard, it is not six keypresses. */
    if (cur[0]==1 && cur[1]==1 && cur[2]==1 && cur[3]==1 && cur[4]==1 && cur[5]==1) {
        return;
    }
    for (int i = 0; i < 6; i++) {
        uint8_t k = cur[i];
        if (k == 0 || k >= 0xE0) continue;   /* empty slot / modifier usage */
        if (!key_was_down(k)) {              /* newly pressed this report */
            kbd_emit(rep[0], k);
        }
    }
    for (int i = 0; i < 6; i++) g_prev_keys[i] = cur[i];
}
/* One poll + edge-decode; returns the next queued character or -1. Shared by the REPL
 * stdin path and the Python usb_kbd.getchar(). */
static int kbd_next_char(int poll_ms) {
    int c = keyq_pop();
    if (c >= 0) return c;
    uint8_t rep[8];
    if (int_in_poll(rep, poll_ms)) process_report(rep);
    return keyq_pop();
}

/* Set the keyboard LEDs (bit0 Num, bit1 Caps, bit2 Scroll) via a control-OUT
 * SET_REPORT on endpoint 0, per keyboard_spec_MI00.md §4: there is no OUT endpoint, so
 * this MUST go over the control pipe. bmRequestType 0x21, SET_REPORT (0x09), wValue
 * 0x0200 (Output report, id 0), wIndex 0 (interface), one data byte. The byte is placed
 * in g_data and byte-swapped so the SoC's hardware DMA word-swap delivers it right (same
 * compensation as every other data buffer). */
void usb_kbd_set_leds(uint8_t leds) {
    if (!g_ready) return;
    g_data[0] = (uint8_t)(leds & 0x07);
    g_data[1] = g_data[2] = g_data[3] = 0;
    bswap32_buf(g_data, 4);
    ctrl_xfer(0x21, 0x09, 0x0200, 0x0000, 1);
}

/* Reset the controller + root-hub port, enumerate and configure the keyboard.
 * Returns 1 on success, 0 if no device / enumeration failed. Plain C so both
 * the Python module and the C REPL stdin path can call it. */
int usb_kbd_bringup(void) {
    g_ready = 0; g_dev_addr = 0; g_int_toggle = 0; g_failstep = 0;
    for (int i = 0; i < 6; i++) g_prev_keys[i] = 0;   /* reset edge-detect state */
    g_keyq_head = g_keyq_tail = 0;
    g_leds = 0;
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
     * USB_HCInit, which we are replacing, so we must do it ourselves.
     *
     * SDIS (Stream Disable, bit 4) is set together with host mode. On a ChipIdea/TDI
     * core, leaving stream mode enabled in host mode lets the controller prefetch and
     * reorder the schedule in a way that corrupts transactions -- the classic symptom is
     * that OUT/SETUP works but every IN retires Halted with no error bits, DEVICE
     * INDEPENDENT. That is exactly what hardware showed: a low-speed keyboard and a
     * high-speed flash drive both stalled the IN with the identical token (B=80080d40),
     * which cannot be two devices coincidentally -- it is the host. Stream Disable is the
     * documented fix and must be set before the schedule runs. */
    EHCI_USBMODE = (EHCI_USBMODE & ~0x00000003u) | USBMODE_CM_HOST | USBMODE_SDIS;
    /* Point the embedded TT at hub address 0x7F, exactly as the firmware does
     * (TTCTRL |= 0x7f0000). Transfers whose QH carries this hub address are routed
     * through the TT, which is how a directly attached low/full-speed device is
     * reached on this core. */
    EHCI_TTCTRL = (EHCI_TTCTRL & ~0x7F000000u) | TTCTRL_TTHA(TT_HUB_ADDR);
    /* And the write the stock firmware actually makes at +0x24: TXFILLTUNING |= 0x7f0000.
     * That is a FIFO-threshold setting, not a TT setting -- it is done here because the
     * firmware does it on this silicon, not because it addresses the TT. */
    EHCI_TXFILLTUNE = EHCI_TXFILLTUNE | 0x007F0000u;
    EHCI_CONFIGFLAG = 1;                 /* route ports to the host controller */
    EHCI_USBCMD = USBCMD_RS;
    for (volatile int i = 0; i < 50000; i++) { }   /* let the port sample D+/D- */

    /* PORT POWER: set PP and give VBUS time to come up before expecting a connect. */
    portsc_rmw(PORTSC_PP, 0);
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
     * needs up to 10ms of recovery (TRSTRCY) before it will answer a SETUP. */
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
     * allowed to fail, so retry a few times before giving up. */
    g_mps0 = 8;
    int got = 0;
    for (int ci = 0; ci < 4; ci++) {
        got = ctrl_xfer(0x80, REQ_GET_DESCRIPTOR, (DESC_DEVICE << 8), 0, 8);
        if (got >= 8) {
            break;
        }
        hc_delay_ms(10);
    }
    if (got < 8) {
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
    usb_kbd_set_leds(0);   /* known LED state (all off) now that EP0 is usable */
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
    /* ~12ms window: long enough for the controller to actually service the interrupt
     * endpoint (which it does about once per frame), short enough that the REPL stays
     * responsive to UART too. Edge-decoded so held/overlapping keys do not double-type. */
    return kbd_next_char(12);
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
    if (int_in_poll(rep, 200)) {
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
    for (int tries = 0; tries < 48; tries++) {
        int c = kbd_next_char(12);
        if (c >= 0) {
            char s[1] = { (char)c };
            return mp_obj_new_str(s, 1);
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(usb_kbd_getchar_obj, usb_kbd_getchar);

/* usb_kbd.leds(mask) -- set the lock LEDs directly (bit0 Num, bit1 Caps, bit2 Scroll).
 * Also updates the internal lock state so Caps affects letter case consistently. */
static mp_obj_t usb_kbd_leds(mp_obj_t mask_obj) {
    if (!g_ready) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("usb_kbd not initialised"));
    }
    g_leds = (uint8_t)(mp_obj_get_int(mask_obj) & 0x07);
    usb_kbd_set_leds(g_leds);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(usb_kbd_leds_obj, usb_kbd_leds);

static const mp_rom_map_elem_t usb_kbd_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_usb_kbd) },
    { MP_ROM_QSTR(MP_QSTR_init),    MP_ROM_PTR(&usb_kbd_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_poll),    MP_ROM_PTR(&usb_kbd_poll_obj) },
    { MP_ROM_QSTR(MP_QSTR_getchar), MP_ROM_PTR(&usb_kbd_getchar_obj) },
    { MP_ROM_QSTR(MP_QSTR_leds),    MP_ROM_PTR(&usb_kbd_leds_obj) },
};
static MP_DEFINE_CONST_DICT(usb_kbd_module_globals, usb_kbd_module_globals_table);

const mp_obj_module_t usb_kbd_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&usb_kbd_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_usb_kbd, usb_kbd_user_cmodule);
