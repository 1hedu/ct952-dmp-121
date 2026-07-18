/*
 * JupiterSDK on CT952 -- OHCI register / descriptor layout (constants only).
 *
 * Pure #defines, no typedefs and no includes, so both the port driver
 * (jusb_ohci.c, via jusb_ohci.h) and the emulator model (ct952emu's
 * machine.c, which uses its own <stdint.h>) can share one authoritative
 * copy of the OHCI 1.0a layout without dragging in either world's types.
 */
#ifndef JUSB_OHCI_REGS_H
#define JUSB_OHCI_REGS_H

/* Register base -- placeholder; the authoritative value lives in the
 * Jungo stack headers (not in this tree). #ifndef-guarded so the real
 * build's value wins. Chosen clear of the backed I/O page and GPIO. */
#ifndef CT909_OHCI_BASE
#define CT909_OHCI_BASE 0x80010000u
#endif

/* Operational registers (offsets from CT909_OHCI_BASE), OHCI 1.0a */
#define OHCI_HcRevision          0x00
#define OHCI_HcControl           0x04
#define OHCI_HcCommandStatus     0x08
#define OHCI_HcInterruptStatus   0x0C
#define OHCI_HcInterruptEnable   0x10
#define OHCI_HcInterruptDisable  0x14
#define OHCI_HcHCCA              0x18
#define OHCI_HcControlHeadED     0x20
#define OHCI_HcControlCurrentED  0x24
#define OHCI_HcDoneHead          0x30
#define OHCI_HcFmNumber          0x3C
#define OHCI_HcPeriodicStart     0x40
#define OHCI_HcRhDescriptorA     0x48
#define OHCI_HcRhStatus          0x50
#define OHCI_HcRhPortStatus1     0x54

/* HcControl */
#define OHCI_CTRL_PLE            (1u << 2)   /* PeriodicListEnable      */
#define OHCI_CTRL_CLE            (1u << 4)   /* ControlListEnable       */
#define OHCI_CTRL_HCFS_SHIFT     6           /* HostControllerFuncState */
#define OHCI_HCFS_OPERATIONAL    2u
/* HcCommandStatus */
#define OHCI_CMD_HCR             (1u << 0)   /* HostControllerReset     */
/* HcInterruptStatus / Enable */
#define OHCI_INT_WDH             (1u << 1)   /* WritebackDoneHead       */
#define OHCI_INT_SF              (1u << 2)   /* StartOfFrame            */
#define OHCI_INT_MIE            (1u << 31)   /* MasterInterruptEnable   */

/* Endpoint Descriptor dword0 (Control) fields */
#define OHCI_ED_FA_SHIFT         0           /* FunctionAddress [6:0]   */
#define OHCI_ED_EN_SHIFT         7           /* EndpointNumber  [10:7]  */
#define OHCI_ED_DIR_SHIFT        11          /* Direction [12:11]: 2=IN */
#define OHCI_ED_DIR_IN           2u
#define OHCI_ED_SPEED            (1u << 13)  /* low speed               */
#define OHCI_ED_SKIP             (1u << 14)
#define OHCI_ED_MPS_SHIFT        16          /* MaxPacketSize [26:16]   */
/* Endpoint Descriptor dword2 (HeadP) low bits */
#define OHCI_ED_HEAD_HALT        1u          /* H                       */
#define OHCI_ED_HEAD_CARRY       2u          /* C (toggleCarry)         */

/* Transfer Descriptor dword0 (Control) fields */
#define OHCI_TD_DP_SHIFT         19          /* Direction/PID [20:19]   */
#define OHCI_TD_DP_IN            2u          /* IN                      */
#define OHCI_TD_CC_SHIFT         28          /* ConditionCode [31:28]   */
#define OHCI_TD_CC_NOTACCESSED   0xEu

/* HCCA layout (256-byte aligned) */
#define OHCI_HCCA_INTTABLE       0           /* 32 ED pointers          */
#define OHCI_HCCA_FRAMENO        128         /* u16 frame number        */
#define OHCI_HCCA_DONEHEAD       132         /* u32 done queue head     */
#define OHCI_HCCA_SIZE           256

/* Bytes of DRAM work area jusb_ohci_init needs (HCCA + ED + 2 TD + buf),
 * from a 256-byte-aligned base. */
#define JUSB_OHCI_WORKAREA       0x140u

/* Boot keyboard's interrupt IN endpoint. Enumeration (stubbed here) would
 * discover this from the interface descriptor; the driver and the model's
 * virtual device agree on it directly. */
#ifndef USB_KBD_ENDPOINT
#define USB_KBD_ENDPOINT         1
#endif

#endif /* JUSB_OHCI_REGS_H */
