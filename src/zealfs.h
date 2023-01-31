/* SPDX-FileCopyrightText: 2022 Zeal 8-bit Computer <contact@zeal8bit.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <assert.h>

/* The default name for the disk image can be provided from the Makefile or command
 * line. If it was not provided, define it here.
 */
#ifndef DEFAULT_IMAGE_NAME
#define DEFAULT_IMAGE_NAME "zfs.img"
#endif

#ifndef DEFAULT_IMAGE_SIZE_KB
#define DEFAULT_IMAGE_SIZE_KB 32
#endif

#ifndef MIN
#define MIN(a,b) (((a) < (b)) ? (a) : (b))
#endif

/**
 * @brief Convert a pointer from the image cache to a page number
 */
#define PTR_TO_IDX(ptr) ((int) ((uintptr_t) ptr - (uintptr_t) g_image))

/* Bit 0 is 1 if is directory */
#define IS_DIR (1 << 0)
/* Bit 7 is 1 if is entry occupied */
#define IS_OCCUPIED (1 << 7)

/* Maximum length of the names in the file system, including th extension */
#define NAME_MAX_LEN 16

/* Type for table entry */
typedef struct {
    /* Bit 0: 1 = directory, 0 = file
     * bit n: reserved
     * Bit 7: 1 = occupied, 0 = free */
    uint8_t flags; /* IS_DIR, IS_FILE, etc... */
    char name[NAME_MAX_LEN];
    uint8_t start_page;
    /* Size of the file in bytes, little-endian! */
    uint16_t size;
    /* Zeal 8-bit OS date format (BCD) */
    uint8_t  year[2];
    uint8_t  month;
    uint8_t  day;
    uint8_t  date;
    uint8_t  hours;
    uint8_t  minutes;
    uint8_t  seconds;
    /* Reserved for future use */
    uint8_t reserved[4];
} __attribute__((packed)) ZealFileEntry;

_Static_assert(sizeof(ZealFileEntry) == 32, "ZealFileEntry must be smaller than 32 bytes");

#define BITMAP_SIZE     32
#define RESERVED_SIZE   28

/* Type for partition header */
typedef struct {
  uint8_t magic;    /* Must be 'Z' ascii code */
  uint8_t version;  /* Version of the file system */
  /* Number of bytes composing the bitmap.
   * No matter how big the disk actually is, the bitmap is always
   * BITMAP_SIZE bytes big, thus we need field to mark the actual
   * number of bytes we will be using. */
  uint8_t bitmap_size;
  /* Number of free pages */
  uint8_t free_pages;
  /* Bitmap for the free pages. A used page is marked as 1, else 0 */
  uint8_t pages_bitmap[BITMAP_SIZE];   /* 256 pages/8-bit = 32 */
  /* Reserved bytes, to align the entries and for future use, such as
   * extended root directory, volume name, extra bitmap, etc... */
  uint8_t reserved[RESERVED_SIZE];
  /* Root directory, must be at an offset multiple of sizeof(ZealFileEntry) */
  ZealFileEntry entries[];
} __attribute__((packed)) ZealFSHeader;

_Static_assert(offsetof(ZealFSHeader, entries) % sizeof(ZealFileEntry) == 0,
               "Root directory entries must be aligned on 32");

/*
 * As the root directory has less available space for the entries, it will have less than
 * regular directories. Define the following macro to simply the calculation.
 */
#define ROOT_MAX_ENTRIES ((256 - sizeof(ZealFSHeader)) / sizeof(ZealFileEntry))

/* Entries count for regular directories (i.e. not root) */
#define DIR_MAX_ENTRIES (256 / sizeof(ZealFileEntry))


/**
 * Help that converting an 8-bit BCD value into a binary value.
 */
static inline int fromBCD(uint8_t value) {
    return (value >> 4) * 10 + (value & 0xf);
}

/**
 * Convert a value between 0 and 99 into a BCD values.
 * For example, if `value` is 13, this function returns 0x13 (in hex!)
 */
static inline uint8_t toBCD(int value) {
    return (((value / 10) % 10) << 4) | (value % 10);
}

/**
 * @brief Free a page in the header bitmap.
 *
 * @param header File system header containing the bitmap to update.
 * @param page Page number to free, must not be 0.
 *
 */
static inline void freePage(ZealFSHeader* header, uint8_t page) {
    assert(page != 0);
    header->pages_bitmap[page / 8] &= ~(1 << page % 8);
    header->free_pages++;
}


/**
 * @brief Allocate one page in the given header's bitmap.
 *
 * @param header File system header to allocate the page from.
 *
 * @return Page number on success, 0 on error.
 */
static uint8_t allocatePage(ZealFSHeader* header) {
  const int size = header->bitmap_size;
  int i = 0;
  uint8_t value = 0;
  for (i = 0; i < size; i++) {
    value = header->pages_bitmap[i];
    if (value != 0xff) {
      break;
    }
  }
  /* If we've reached the size, the bitmap is full */
  if (i == size) {
    return 0;
  }
  /* Else, return the index */
  int index_0 = 0;
  while ((value & 1) != 0) {
    value >>= 1;
    index_0++;
  }

  /* Set the page as allocated in the bitmap */
  header->pages_bitmap[i] |= 1 << index_0;
  header->free_pages--;
  return i * 8 + index_0;
}