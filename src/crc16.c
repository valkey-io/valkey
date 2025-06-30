#include "server.h"

/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 */


/* Adapted from: https://www.eevblog.com/forum/programming/crc32-half-bytenibble-lookup-table-algorithm/
 * The lookup table just needs to store the entries from 1st to 16th
 * of the standard lookup table.
 */
const uint16_t crc16_tbl[16] = {
    0x0000,
    0x1021,
    0x2042,
    0x3063,
    0x4084,
    0x50a5,
    0x60c6,
    0x70e7,
    0x8108,
    0x9129,
    0xa14a,
    0xb16b,
    0xc18c,
    0xd1ad,
    0xe1ce,
    0xf1ef,
};

/* Adapted from: https://create.stephan-brumme.com/crc32/
 * Apply half-byte lookup table algorithm for the balance
 * between performance and the size of lookup table.
 */
static inline uint16_t crc16_base(uint16_t crc, uint8_t v) {
    crc ^= v << 8;
    crc = (crc << 4) ^ crc16_tbl[crc >> 12];
    crc = (crc << 4) ^ crc16_tbl[crc >> 12];
    return crc;
}

/* CRC16 implementation according to CCITT standards.
 *
 * Note by @antirez: this is actually the XMODEM CRC 16 algorithm, using the
 * following parameters:
 *
 * Name                       : "XMODEM", also known as "ZMODEM", "CRC-16/ACORN"
 * Width                      : 16 bit
 * Poly                       : 1021 (That is actually or x^16 + x^12 + x^5 + 1)
 * Initialization             : 0000
 * Reflect Input byte         : False
 * Reflect Output CRC         : False
 * Xor constant to output CRC : 0000
 * Output for "123456789"     : 31C3
 */
uint16_t crc16(const char *buf, int len) {
    int counter;
    uint16_t crc = 0;
    for (counter = 0; counter < len; counter++) {
        uint8_t tmp = (uint8_t)buf[counter];
        crc = crc16_base(crc, tmp);
    }
    return crc;
}
