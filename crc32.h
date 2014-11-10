#ifndef _CRC32_H
#define _CRC32_H

#include <stdint.h>
#include <stddef.h>

uint32_t psi_crc32(uint8_t *data, size_t datalen, uint32_t crc);

#endif
