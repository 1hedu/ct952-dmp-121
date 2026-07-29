/* OSD-register Rosetta probe: call the firmware's GDI to program OSD region 0
 * as a known 616x78 4bpp plane, then spin. Purpose is NOT to draw -- it is to
 * let DISP_OSDSet / DISP_OSDSetColorMode write the real OSD-channel registers so
 * the emulator's CT952_DUMP_OSD can read back exactly which reg holds base,
 * width, stride, and colormode. Feeds the faithful DISP scanout model. */
#include <stdint.h>
typedef uint16_t WORD; typedef uint8_t BYTE; typedef uint32_t DWORD;
typedef struct { DWORD wWidth, wHeight; BYTE bColorMode; DWORD dwTAddr; } GDI_REGION_INFO;

#define GDI_ConfigRegionInfo ((void (*)(BYTE, GDI_REGION_INFO *))0x00007798u)
#define GDI_InitialRegion    ((void (*)(BYTE))0x00007818u)
#define GDI_ClearRegion      ((void (*)(BYTE))0x000078b4u)
#define GDI_ActivateRegion   ((void (*)(BYTE))0x00007904u)

int pyapp_main(void){
    GDI_REGION_INFO info;
    info.wWidth = 616; info.wHeight = 78; info.bColorMode = 1; info.dwTAddr = 0x40084000u;
    GDI_ConfigRegionInfo(0, &info);
    GDI_InitialRegion(0);
    GDI_ClearRegion(0);
    GDI_ActivateRegion(0);
    for (;;){}
    return 0;
}
