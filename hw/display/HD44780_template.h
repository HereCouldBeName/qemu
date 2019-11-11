/*
 * QEMU Macintosh 128k draw line template
 *
 * Copyright (c) 2015 Pavel Dovgalyuk
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#if DEPTH == 8
# define BPP 1
# define PIXEL_TYPE uint8_t
#elif DEPTH == 15 || DEPTH == 16
# define BPP 2
# define PIXEL_TYPE uint16_t
#elif DEPTH == 32
# define BPP 4
# define PIXEL_TYPE uint32_t
#else
# error unsupport depth
#endif

#define SCALE 4

/*
 * 1-bit colour
 */
static void glue(draw_char_, DEPTH)(const uint8_t *s, DisplaySurface *surface, int column, int line)
{
    uint8_t r, g, b, mask;
    uint8_t *dest;

    dest = surface_data(surface) + (column * 6 * BPP * SCALE) + (line * surface_stride(surface) * (9 * SCALE)) +
            (surface_stride(surface)*SCALE*2) + (BPP*SCALE*2);
    for(uint8_t l=0; l<7; l++) {
        for(uint8_t ls=0; ls<SCALE; ls++) {
            for (mask = 0x10 ; mask ; mask >>= 1) {
                for(uint8_t ws=0; ws<SCALE; ws++) {
                    r = (*s & mask) ? 0 : 0xff;
                    g = (*s & mask) ? 0 : 0xff;
                    b = (*s & mask) ? 0 : 0xff;
                    ((PIXEL_TYPE *) dest)[0] = glue(rgb_to_pixel, DEPTH)(r, g, b);
                    dest += BPP;
                }
            }
            dest += surface_stride(surface) - BPP * SCALE * 5;
        }
        s++;
    }
}

static void glue(draw_cursor_, DEPTH)(DisplaySurface *surface, int column, int line, int option, bool is_square)
{
    uint8_t r = option, g = option, b = option;
    uint8_t *dest;

    dest = surface_data(surface) + (column * 6 * BPP * SCALE) + (line * surface_stride(surface) * (9 * SCALE)) +
            (surface_stride(surface)*SCALE*2) + (BPP*SCALE*2);
    uint8_t count_line = 8;
    if (!is_square) {
        dest += surface_stride(surface) * (8 * SCALE);
        count_line = 1;
    }
    for(uint8_t l=0; l<count_line; l++) {
        for(uint8_t ls = 0; ls < SCALE; ls++) {
            for(uint8_t i = 0; i < 5*SCALE; i++) {
                ((PIXEL_TYPE *) dest)[0] = glue(rgb_to_pixel, DEPTH)(r, g, b);
                dest += BPP;
            }
            dest += surface_stride(surface) - BPP * SCALE * 5;
        }
    }
//    dest = surface_data(surface) + (column * 6 * BPP * SCALE) + (line * surface_stride(surface) * (9 * SCALE)) +
//            (surface_stride(surface)*SCALE*2) + (BPP*SCALE*2) + surface_stride(surface) * (8 * SCALE);
}
#undef DEPTH
#undef BPP
#undef PIXEL_TYPE
