#include <stdint.h>
#include "wdg.h"

#define R(off) (*(volatile uint32_t *)(0x40020000u + (off)))

void gp5_wdg_disable(void)
{
    R(0x04) = 1;
    R(0x0C) = 1;
}

void gp5_wdg_enable_1s(void)
{
#ifdef GP50_BOARD
    R(0x10) &= ~(1u << 3);     /* V1.0.5 loader handover */
#else
    R(0x10) &= ~3u;            /* period field: 0 = 1 s */
#endif
    R(0x10) |= 0u;
    R(0x14) = 1;
    R(0x18) = 1;
    R(0x04) = 1;
    R(0x08) = 1;
}
