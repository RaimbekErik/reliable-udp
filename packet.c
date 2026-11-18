/* packet.c - serialize / parse / verify packets.
 *
 * Wire layout (12-byte header, all multi-byte fields big-endian):
 *   [0]     type   (PKT_DATA / PKT_ACK / PKT_FIN / PKT_FINACK)
 *   [1]     flags  (reserved, 0)
 *   [2..3]  len    payload length in bytes
 *   [4..7]  num    sequence number (DATA/FIN) or ACK number (ACK)
 *   [8..11] crc    CRC-32 over header (with this field = 0) + payload
 *   [12..]  payload
 */
#include <string.h>
#include "common.h"

static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8  | (uint32_t)p[3];
}

size_t pkt_pack(uint8_t *buf, uint8_t type, uint32_t num,
                const uint8_t *payload, uint16_t len)
{
    buf[0] = type;
    buf[1] = 0;
    put16(buf + 2, len);
    put32(buf + 4, num);
    put32(buf + 8, 0);                       /* CRC field is 0 while computing */
    if (len > 0)
        memcpy(buf + HDR_SIZE, payload, len);
    put32(buf + 8, crc32(buf, HDR_SIZE + len));
    return HDR_SIZE + len;
}

int pkt_unpack(const uint8_t *buf, size_t n, pkt_hdr *h,
               const uint8_t **payload)
{
    if (n < HDR_SIZE || n > HDR_SIZE + MAX_PAYLOAD)
        return -1;

    uint16_t len = get16(buf + 2);
    if ((size_t)HDR_SIZE + len != n)         /* corrupted length field */
        return -1;

    uint8_t tmp[HDR_SIZE + MAX_PAYLOAD];
    memcpy(tmp, buf, n);
    uint32_t stored = get32(tmp + 8);
    put32(tmp + 8, 0);
    if (crc32(tmp, n) != stored)
        return -1;

    h->type  = buf[0];
    h->flags = buf[1];
    h->len   = len;
    h->num   = get32(buf + 4);
    h->crc   = stored;
    if (h->type > PKT_FINACK)                /* unknown type */
        return -1;
    if (payload)
        *payload = buf + HDR_SIZE;
    return 0;
}
