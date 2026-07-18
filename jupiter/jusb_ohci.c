/*
 * JupiterSDK on CT952 -- USB OHCI boot-keyboard transport (driver side).
 *
 * Enumerates a boot keyboard over the OHCI control list, then polls its
 * interrupt-IN endpoint for the 8-byte report each frame. Polled: no
 * interrupt handler; jusb_ohci_poll() reads WritebackDoneHead.
 *
 * DRAM work-area layout (from a 256-byte-aligned base):
 *   +0x000  HCCA            (256 bytes, 256-aligned)
 *   +0x100  periodic ED     (interrupt IN)
 *   +0x110  periodic TD0    (active IN transfer)
 *   +0x120  periodic TD1    (dummy tail)
 *   +0x130  report buffer   (8 bytes)
 *   +0x140  control ED      (endpoint 0)
 *   +0x150  control TD0..3  (SETUP / DATA / STATUS / dummy tail)
 *   +0x190  setup packet    (8 bytes)
 *   +0x1A0  control data buf (64 bytes)
 *
 * Enumeration issues each control transfer as a SETUP(+DATA)+STATUS TD
 * chain on the control ED and spins for WritebackDoneHead. The endpoint
 * the keyboard is finally polled on comes from its configuration
 * descriptor, not from an assumed constant.
 */
#include "jusb_ohci.h"

#define OHCIREG(off) (*(volatile uint32_t *)(CT909_OHCI_BASE + (off)))

static uint32_t s_hcca, s_ped, s_ptd0, s_ptd1, s_buf;
static uint32_t s_ced, s_ctd0, s_ctd1, s_ctd2, s_ctd3, s_setup, s_data;
static uint32_t s_addr;      /* device address currently assigned */
static uint32_t s_kbd_ep;    /* interrupt IN endpoint discovered */

static void wr32(uint32_t addr, uint32_t v) { *(volatile uint32_t *)addr = v; }

/* Rebuild the periodic TD0 as a fresh IN transfer into the report buffer. */
static void arm_td(void)
{
    wr32(s_ptd0 + 0,  (OHCI_TD_CC_NOTACCESSED << OHCI_TD_CC_SHIFT) |
                      (OHCI_TD_DP_IN << OHCI_TD_DP_SHIFT));
    wr32(s_ptd0 + 4,  s_buf);
    wr32(s_ptd0 + 8,  s_ptd1);
    wr32(s_ptd0 + 12, s_buf + 7);
    wr32(s_ped  + 8,  s_ptd0);        /* HeadP = TD0 (H=0, C=0) */
}

/* Compose an 8-byte SETUP packet in the setup buffer. */
static void put_setup(uint8_t bmr, uint8_t req,
                      uint16_t wval, uint16_t widx, uint16_t wlen)
{
    volatile uint8_t *p = (volatile uint8_t *)s_setup;
    p[0] = bmr;              p[1] = req;
    p[2] = (uint8_t)wval;    p[3] = (uint8_t)(wval >> 8);
    p[4] = (uint8_t)widx;    p[5] = (uint8_t)(widx >> 8);
    p[6] = (uint8_t)wlen;    p[7] = (uint8_t)(wlen >> 8);
}

/* Run one control transfer to completion: SETUP (+ optional DATA) + STATUS
 * on the control ED, kick the control list, spin for WritebackDoneHead. */
static void ctrl_xfer(uint32_t data_buf, uint32_t data_len, int data_in)
{
    uint32_t cc = (OHCI_TD_CC_NOTACCESSED << OHCI_TD_CC_SHIFT);
    long g = 0;

    /* SETUP TD */
    wr32(s_ctd0 + 0,  cc | (0u << OHCI_TD_DP_SHIFT));
    wr32(s_ctd0 + 4,  s_setup);
    wr32(s_ctd0 + 12, s_setup + 7);
    if (data_len) {
        wr32(s_ctd0 + 8, s_ctd1);                /* SETUP -> DATA */
        wr32(s_ctd1 + 0, cc | ((data_in ? OHCI_TD_DP_IN : 1u)
                               << OHCI_TD_DP_SHIFT));
        wr32(s_ctd1 + 4, data_buf);
        wr32(s_ctd1 + 8, s_ctd2);                /* DATA -> STATUS */
        wr32(s_ctd1 + 12, data_buf + data_len - 1);
    } else {
        wr32(s_ctd0 + 8, s_ctd2);                /* SETUP -> STATUS */
    }
    /* STATUS TD: zero-length, opposite direction (IN read -> OUT status). */
    wr32(s_ctd2 + 0,  cc | ((data_in ? 1u : OHCI_TD_DP_IN) << OHCI_TD_DP_SHIFT));
    wr32(s_ctd2 + 4,  0);
    wr32(s_ctd2 + 8,  s_ctd3);
    wr32(s_ctd2 + 12, 0);

    /* Control ED: FA=current address, EN=0, direction from TD, LS, MPS=8. */
    wr32(s_ced + 0, ((uint32_t)s_addr << OHCI_ED_FA_SHIFT) |
                    OHCI_ED_SPEED | (8u << OHCI_ED_MPS_SHIFT));
    wr32(s_ced + 4, s_ctd3);         /* TailP = dummy */
    wr32(s_ced + 8, s_ctd0);         /* HeadP = SETUP TD */
    wr32(s_ced + 12, 0);

    OHCIREG(OHCI_HcControlHeadED) = s_ced;
    OHCIREG(OHCI_HcControl) =
        (OHCI_HCFS_OPERATIONAL << OHCI_CTRL_HCFS_SHIFT) | OHCI_CTRL_CLE;

    while (!(OHCIREG(OHCI_HcInterruptStatus) & OHCI_INT_WDH) && g < 20000000L)
        g++;
    wr32(s_hcca + OHCI_HCCA_DONEHEAD, 0);
    OHCIREG(OHCI_HcInterruptStatus) = OHCI_INT_WDH;   /* ack */
}

/* Find the first interrupt IN endpoint in the configuration descriptor. */
static int parse_endpoint(uint32_t buf, uint32_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)buf;
    uint32_t pos = 0;
    while (pos + 2 <= len) {
        uint8_t blen = p[pos], btype = p[pos + 1];
        if (blen == 0) break;
        if (btype == 0x05 && pos + 3 <= len && (p[pos + 2] & 0x80u))
            return p[pos + 2] & 0x0Fu;           /* IN endpoint number */
        pos += blen;
    }
    return -1;
}

int jusb_ohci_init(uint32_t work)
{
    int ep, i;

    s_hcca  = work;          s_ped  = work + 0x100; s_ptd0 = work + 0x110;
    s_ptd1  = work + 0x120;  s_buf  = work + 0x130; s_ced  = work + 0x140;
    s_ctd0  = work + 0x150;  s_ctd1 = work + 0x160; s_ctd2 = work + 0x170;
    s_ctd3  = work + 0x180;  s_setup = work + 0x190; s_data = work + 0x1A0;
    s_addr  = 0;

    for (i = 0; i < OHCI_HCCA_SIZE / 4; i++) wr32(s_hcca + i * 4, 0);

    /* Start the controller with HCCA set but no lists enabled yet. */
    OHCIREG(OHCI_HcHCCA)            = s_hcca;
    OHCIREG(OHCI_HcInterruptStatus) = 0xFFFFFFFFu;
    OHCIREG(OHCI_HcControl)         = OHCI_HCFS_OPERATIONAL << OHCI_CTRL_HCFS_SHIFT;

    /* --- enumeration --- */
    put_setup(0x80, 0x06, 0x0100, 0, 8);   ctrl_xfer(s_data, 8, 1);   /* GET dev  */
    put_setup(0x00, 0x05, 1, 0, 0);        ctrl_xfer(0, 0, 0);        /* SET_ADDR */
    s_addr = 1;
    put_setup(0x80, 0x06, 0x0200, 0, 34);  ctrl_xfer(s_data, 34, 1);  /* GET cfg  */
    ep = parse_endpoint(s_data, 34);
    if (ep < 0) return -1;
    put_setup(0x00, 0x09, 1, 0, 0);        ctrl_xfer(0, 0, 0);        /* SET_CFG  */
    put_setup(0x21, 0x0B, 0, 0, 0);        ctrl_xfer(0, 0, 0);        /* SET_PROTO boot */

    /* --- periodic interrupt-IN endpoint (discovered) --- */
    s_kbd_ep = (uint32_t)ep;
    wr32(s_ped + 0, ((uint32_t)s_addr << OHCI_ED_FA_SHIFT) |
                    (s_kbd_ep << OHCI_ED_EN_SHIFT) |
                    (OHCI_ED_DIR_IN << OHCI_ED_DIR_SHIFT) |
                    OHCI_ED_SPEED | (8u << OHCI_ED_MPS_SHIFT));
    wr32(s_ped + 4, s_ptd1);         /* TailP */
    wr32(s_ped + 12, 0);
    arm_td();
    for (i = 0; i < 32; i++)
        wr32(s_hcca + OHCI_HCCA_INTTABLE + i * 4, s_ped);

    OHCIREG(OHCI_HcControl) =
        (OHCI_HCFS_OPERATIONAL << OHCI_CTRL_HCFS_SHIFT) | OHCI_CTRL_PLE;
    return ep;
}

int jusb_ohci_poll(uint8_t report[8])
{
    volatile uint8_t *b = (volatile uint8_t *)s_buf;
    int i;

    if (!(OHCIREG(OHCI_HcInterruptStatus) & OHCI_INT_WDH))
        return 0;

    for (i = 0; i < 8; i++)
        report[i] = b[i];

    arm_td();
    wr32(s_hcca + OHCI_HCCA_DONEHEAD, 0);
    OHCIREG(OHCI_HcInterruptStatus) = OHCI_INT_WDH;
    return 1;
}
