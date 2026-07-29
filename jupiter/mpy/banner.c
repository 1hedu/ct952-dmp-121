/* Bring-up via the firmware's OWN GDI, called from the bare-metal AP.
 *
 * Manual pixel-poking into 0x40084000 keeps rendering garbled, so stop guessing
 * the pixel format and call the firmware's renderer -- the exact code that draws
 * the loader's own text, so it uses the real stride/geometry by construction.
 *
 * gdi.c is compiled with GDI_WITHOUT_OS + GDI_STANDALONE_LIBRARY (gdi.c:25-26):
 * no mutexes, no OS, no OSD-module calls in the draw path -- safe to call bare
 * metal. Addresses are XIP offsets from the firmware symbol map, verified by
 * disassembling dp700wd.bin (chip = CT952A; the map is DVD909-toolchain lineage,
 * but the bytes at these offsets are the matching GDI functions).
 *
 * We replicate aploader.c STEP2 (lines 116-123) exactly: configure region 0 as
 * 616x78 4bpp @ 0x40084000, GDI_InitialRegion (programs the DISP block via
 * DISP_OSDSet), clear, activate -- then GDI_FillRect two rectangles. If the panel
 * shows two crisp, correctly-placed rectangles (not sheared), the firmware-call
 * path works and the region config is sound; text via GDI_DrawString is next.
 */
#include <stdint.h>

typedef uint16_t WORD;
typedef uint8_t  BYTE;
typedef uint32_t DWORD;

/* firmware struct layouts (gdi.h / comdef.h), same m32 BE ABI */
typedef struct { DWORD wWidth, wHeight; BYTE bColorMode; DWORD dwTAddr; } GDI_REGION_INFO;
typedef struct { WORD wLeft, wTop, wRight, wBottom; } URECT;
typedef struct { URECT rect; BYTE bColor; BYTE *bShadePtr; } PARM_RECT;

/* firmware GDI entry points (XIP, verified vs dp700wd.bin) */
#define GDI_ConfigRegionInfo ((void (*)(BYTE, GDI_REGION_INFO *))0x00007798u)
#define GDI_InitialRegion    ((void (*)(BYTE))0x00007818u)
#define GDI_ClearRegion      ((void (*)(BYTE))0x000078b4u)
#define GDI_ActivateRegion   ((void (*)(BYTE))0x00007904u)
#define GDI_FillRect         ((void (*)(BYTE, PARM_RECT *))0x00007cdcu)

#define REG_CACHE (*(volatile uint32_t *)0x80000014u)

static void flush(void){
    REG_CACHE &= ~0x00040000u; REG_CACHE |= 0x00400000u;
    for (volatile int i = 0; i < 256; i++) __asm__ __volatile__("nop");
    REG_CACHE |= 0x00040000u;
}

static void fillrect(BYTE l, BYTE t, WORD r, BYTE b, BYTE color){
    PARM_RECT p;
    p.rect.wLeft = l; p.rect.wTop = t; p.rect.wRight = r; p.rect.wBottom = b;
    p.bColor = color; p.bShadePtr = 0;
    GDI_FillRect(0, &p);
}

int pyapp_main(void){
    GDI_REGION_INFO info;
    info.wWidth = 616; info.wHeight = 78; info.bColorMode = 1; /* GDI_OSD_4B_MODE */
    info.dwTAddr = 0x40084000u;                                /* DS_OSDFRAME_ST_AP */

    GDI_ConfigRegionInfo(0, &info);
    GDI_InitialRegion(0);
    GDI_ClearRegion(0);
    GDI_ActivateRegion(0);

    /* outer rectangle (index 2) with an inner rectangle (index 1) inside it --
     * both drawn by the firmware's own _gdi_FillRect at the real stride */
    fillrect(8,  6, 300, 50, 2);
    fillrect(40, 18, 120, 38, 1);

    flush();
    for (;;){}
    return 0;
}
