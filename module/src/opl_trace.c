/*
 * opl_trace.c -- the AdLib chip for Wolf4SDL's id_sd.c: Nuked OPL3 (../third_party/nuked-opl3, GPL-2+, the same
 * emulator Crispy Doom uses) behind the three YM3812 calls id_sd.c makes.
 *
 * Why not Wolf4SDL's own choices: its MAME emulator (mame/fmopl.c) is under a non-commercial licence that isn't
 * GPL-compatible, and its GPL one (USE_GPL, DOSBox's dbopl.cpp) no longer builds since Wolf4SDL moved from C++ to C
 * (id_sd.c still calls it as C++). So id_sd.c is built on its MAME path, which only declares these three functions
 * (mame/fmopl.h), and they are answered here; no MAME code is compiled.
 *
 * id_sd.c's contract (the MAME version Wolf4SDL patched): UpdateOne writes `length` stereo frames, left and right
 * interleaved, at the rate given to Init.
 */

#include <stdint.h>

#include "opl3.h"

static opl3_chip g_chip;

int YM3812Init(int num, int clock, int rate)
{
    (void)num; (void)clock;
    OPL3_Reset(&g_chip, (Bit32u)rate);
    return 0;       /* 0 = success, as id_sd.c checks it */
}

void YM3812Shutdown(void) { }

int YM3812Write(int which, int a, int v)
{
    (void)which;
    OPL3_WriteReg(&g_chip, (Bit16u)a, (Bit8u)v);
    return 0;
}

void YM3812UpdateOne(int which, int16_t *buffer, int length)
{
    (void)which;
    if (length > 0)
        OPL3_GenerateStream(&g_chip, buffer, (Bit32u)length);
}
