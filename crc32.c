#include "common.h"

#define CRC32_POLY 0xEDB88320u          /* IEEE 802.3 polynomial, bit-reversed */

static uint32_t table[256];             /* precomputed results for every byte value */
static int ready = 0;                   /* has the table been built yet? */

static void init_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int bit = 0; bit < 8; bit++) {
            if (c & 1u)
                c = (c >> 1) ^ CRC32_POLY;   /* low bit 1: shift and "subtract" poly */
            else
                c = c >> 1;                  /* low bit 0: just shift */
        }
        table[i] = c;
    }
    ready = 1;
}

uint32_t crc32(const uint8_t *data, size_t n)
{
    if (!ready)
        init_table();

    uint32_t crc = 0xFFFFFFFFu;                         /* standard initial value */
    for (size_t i = 0; i < n; i++)
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;                           /* standard final XOR */
}