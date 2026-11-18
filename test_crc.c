/* test_crc.c - unit tests for crc32.c and packet.c */
#include <stdio.h>
#include <string.h>
#include "common.h"

static int fails = 0;
#define CHECK(cond, name) do { \
    printf("%-50s %s\n", name, (cond) ? "OK" : "FAIL"); \
    if (!(cond)) fails++; } while (0)

int main(void)
{
    /* 1. Standard CRC-32 check values */
    const char *s = "123456789";
    CHECK(crc32((const uint8_t *)s, 9) == 0xCBF43926u, "crc32(\"123456789\") == 0xCBF43926");
    CHECK(crc32((const uint8_t *)"", 0) == 0x00000000u, "crc32(\"\") == 0x00000000");
    CHECK(crc32((const uint8_t *)"a", 1) == 0xE8B7BE43u, "crc32(\"a\") == 0xE8B7BE43");

    /* 2. Pack -> unpack round trip */
    uint8_t buf[HDR_SIZE + MAX_PAYLOAD];
    pkt_hdr h;
    const uint8_t *pl;
    size_t n = pkt_pack(buf, PKT_DATA, 5, (const uint8_t *)"hello", 5);
    CHECK(n == HDR_SIZE + 5, "packed size == 17");
    int r = pkt_unpack(buf, n, &h, &pl);
    CHECK(r == 0 && h.type == PKT_DATA && h.num == 5 && h.len == 5 &&
          memcmp(pl, "hello", 5) == 0, "DATA round trip");

    /* ACK with empty payload */
    n = pkt_pack(buf, PKT_ACK, 127, NULL, 0);
    CHECK(pkt_unpack(buf, n, &h, NULL) == 0 && h.type == PKT_ACK && h.num == 127,
          "ACK (no payload) round trip");

    /* Full-size payload */
    uint8_t big[MAX_PAYLOAD];
    for (int i = 0; i < MAX_PAYLOAD; i++) big[i] = (uint8_t)(i * 7);
    n = pkt_pack(buf, PKT_DATA, 42, big, MAX_PAYLOAD);
    CHECK(pkt_unpack(buf, n, &h, &pl) == 0 && memcmp(pl, big, MAX_PAYLOAD) == 0,
          "1024-byte payload round trip");

    /* 3. Every single-bit flip, anywhere in header or payload, is detected */
    int missed = 0;
    for (size_t byte = 0; byte < n; byte++)
        for (int bit = 0; bit < 8; bit++) {
            buf[byte] ^= (uint8_t)(1u << bit);
            if (pkt_unpack(buf, n, &h, &pl) == 0) missed++;
            buf[byte] ^= (uint8_t)(1u << bit);   /* restore */
        }
    printf("  (tested %zu single-bit flips, %d undetected)\n", n * 8, missed);
    CHECK(missed == 0, "all single-bit corruptions detected");

    /* 4. Truncated / oversized datagrams rejected */
    CHECK(pkt_unpack(buf, 5, &h, &pl) == -1, "datagram shorter than header rejected");
    CHECK(pkt_unpack(buf, n - 1, &h, &pl) == -1, "length mismatch rejected");

    printf("\n%s (%d failure%s)\n", fails ? "SOME TESTS FAILED" : "ALL TESTS PASSED",
           fails, fails == 1 ? "" : "s");
    return fails != 0;
}
