/* SPDX-FileCopyrightText: 2025 Zeal 8-bit Computer <contact@zeal8bit.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <fcntl.h>

#define MBR_SIZE    512

int mbr_create(uint8_t* out_mbr, off_t part_offset, size_t part_size);

int mbr_find_partition(const char* filename, int filesize, off_t* offset, int* size);