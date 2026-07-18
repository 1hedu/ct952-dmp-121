/*
 * JupiterSDK on CT952 -- USB OHCI boot-keyboard transport (driver side).
 *
 * Programs the OHCI periodic list with a single interrupt-IN endpoint and
 * harvests the 8-byte report each frame. Polled: no interrupt handler is
 * installed; jusb_ohci_poll() reads the WritebackDoneHead status.
 *
 * DRAM work-area layout (from a 256-byte-aligned base):
 *   +0x000  HCCA           (256 bytes, 256-aligned)
 *   +0x100  ED             (16 bytes, 16-aligned)
 *   +0x110  TD0            (16 bytes) -- the active IN transfer
 *   +0x120  TD1            (16 bytes) -- dummy tail
 *   +0x130  report buffer  (8 bytes)
 *
 * Re-arm is a simplified single-endpoint scheme: on completion the driver
 * rebuilds TD0 and resets ED HeadP to it. A production multi-endpoint
 * driver would append TDs at a dummy tail instead; the controller model
 * is agnostic to which, so this keeps the transport minimal and robust.
 */
#include "jusb_ohci.h"

#define OHCIREG(off) (*(volatile uint32_t *)(CT909_OHCI_BASE + (off)))

static uint32_t s_hcca, s_ed, s_td0, s_td1, s_buf;

static void wr32(uint32_t addr, uint32_t v)
{
    *(volatile uint32_t *)addr = v;
}
static uint32_t rd32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

/* Build TD0 as a fresh IN transfer into the report buffer. */
static void arm_td(void)
{
    wr32(s_td0 + 0,  (OHCI_TD_CC_NOTACCESSED << OHCI_TD_CC_SHIFT) |
                     (OHCI_TD_DP_IN << OHCI_TD_DP_SHIFT));
    wr32(s_td0 + 4,  s_buf);          /* CBP: current buffer pointer   */
    wr32(s_td0 + 8,  s_td1);          /* NextTD: the dummy tail        */
    wr32(s_td0 + 12, s_buf + 7);      /* BE: last byte of the 8-byte buf */
    wr32(s_ed  + 8,  s_td0);          /* ED HeadP = TD0 (H=0, C=0)     */
}

int jusb_ohci_init(uint32_t dram_workarea)
{
    int i;

    s_hcca = dram_workarea;
    s_ed   = dram_workarea + 0x100;
    s_td0  = dram_workarea + 0x110;
    s_td1  = dram_workarea + 0x120;
    s_buf  = dram_workarea + 0x130;

    /* Clear HCCA and the report buffer. */
    for (i = 0; i < OHCI_HCCA_SIZE / 4; i++) wr32(s_hcca + i * 4, 0);
    wr32(s_buf + 0, 0);
    wr32(s_buf + 4, 0);

    /* Endpoint descriptor: default address 0, endpoint USB_KBD_ENDPOINT,
     * IN, low speed, 8-byte max packet. TailP = dummy tail TD1. */
    wr32(s_ed + 0, (0u << OHCI_ED_FA_SHIFT) |
                   ((uint32_t)USB_KBD_ENDPOINT << OHCI_ED_EN_SHIFT) |
                   (OHCI_ED_DIR_IN << OHCI_ED_DIR_SHIFT) |
                   OHCI_ED_SPEED |
                   (8u << OHCI_ED_MPS_SHIFT));
    wr32(s_ed + 4, s_td1);            /* TailP */
    wr32(s_ed + 12, 0);              /* NextED: none */
    arm_td();                        /* builds TD0, sets HeadP */

    /* Link the ED into every interrupt-table slot so it is serviced each
     * frame regardless of which slot the controller picks. */
    for (i = 0; i < 32; i++)
        wr32(s_hcca + OHCI_HCCA_INTTABLE + i * 4, s_ed);

    /* Program and start the controller. */
    OHCIREG(OHCI_HcHCCA)           = s_hcca;
    OHCIREG(OHCI_HcInterruptStatus) = 0xFFFFFFFFu;   /* clear stale status */
    OHCIREG(OHCI_HcControl)        = (OHCI_HCFS_OPERATIONAL << OHCI_CTRL_HCFS_SHIFT) |
                                     OHCI_CTRL_PLE;
    return 0;
}

int jusb_ohci_poll(uint8_t report[8])
{
    volatile uint8_t *b = (volatile uint8_t *)s_buf;
    int i;

    if (!(OHCIREG(OHCI_HcInterruptStatus) & OHCI_INT_WDH))
        return 0;                    /* no completed transfer */

    for (i = 0; i < 8; i++)
        report[i] = b[i];

    /* Recycle: rebuild TD0, clear the done head, ack WDH (write-1-clear). */
    arm_td();
    wr32(s_hcca + OHCI_HCCA_DONEHEAD, 0);
    OHCIREG(OHCI_HcInterruptStatus) = OHCI_INT_WDH;
    return 1;
}
