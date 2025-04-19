/* SPDX-FileCopyrightText: 2022 Zeal 8-bit Computer <contact@zeal8bit.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <assert.h>


#define BIT(X)  (1ULL << (X))
#define KB(X)   (X)*1024ULL
#define MB(X)   KB(X)*1024ULL
#define GB(X)   MB(X)*1024ULL


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
    uint16_t start_page;
    /* Size of the file in bytes, little-endian! */
    uint32_t size;
    /* Zeal 8-bit OS date format (BCD) */
    uint8_t  year[2];
    uint8_t  month;
    uint8_t  day;
    uint8_t  date;
    uint8_t  hours;
    uint8_t  minutes;
    uint8_t  seconds;
    /* Reserved for future use */
    uint8_t reserved;
} __attribute__((packed)) ZealFileEntry;

_Static_assert(sizeof(ZealFileEntry) == 32, "ZealFileEntry must be smaller than 32 bytes");

#define RESERVED_SIZE   28

/* Type for partition header */
typedef struct {
  uint8_t magic;    /* Must be 'Z' ascii code */
  uint8_t version;  /* Version of the file system */
  /* Number of bytes composing the bitmap */
  uint16_t bitmap_size;
  /* Number of free pages, we always have at most 65535 pages */
  uint16_t free_pages;
  /* Size of the pages:
   * 0 - 256
   * 1 - 512
   * 2 - 1024
   * 3 - 2048
   * 4 - 4096
   * 5 - 8192
   * 6 - 16384
   * 7 - 32768
   * 8 - 65536
   */
  uint8_t page_size;
  /* Bitmap for the free pages. A used page is marked as 1, else 0 */
  uint8_t pages_bitmap[];   /* 256 pages/8-bit = 32 */
} __attribute__((packed)) ZealFSHeader;


#define ALIGN_UP(size,bound) (((size) + (bound) - 1) & ~((bound) - 1))

/**
 * @brief Get the size of the FileSystem header, multiple of 32 (sizeof(ZealFileEntry))
 */
static inline int getFSHeaderSize(const ZealFSHeader* header)
{
    int size = sizeof(ZealFSHeader) + header->bitmap_size;
    /* Round the size up to the next sizeof(ZealFileEntry) bytes bound (FileEntry) */
    size = ALIGN_UP(size, sizeof(ZealFileEntry));
    return size;
}


/**
 * @brief Get the size, in bytes, of the pages on the current disk
 */
static inline int getPageSize(const ZealFSHeader* header)
{
    const int size = header->page_size;
    assert (size <= 8);
    return 256 << size;
}


/**
 * Helper to get a pointer to the root directory entries
 */
static inline ZealFileEntry* getRootDirEntries(ZealFSHeader* header) {
    return ((void*) header) + getFSHeaderSize(header);
}


/*
 * As the root directory has less available space for the entries, it will have less than
 * regular directories. Define the following macro to simply the calculation.
 */
static inline uint16_t getRootDirMaxEntries(ZealFSHeader* header) {
    /* The size of the header depends on the size of the bitmap */
    return (getPageSize(header) - getFSHeaderSize(header)) / sizeof(ZealFileEntry);
}


/**
 * Get the maximum number of files in other directories than the root one
 */
static inline uint16_t getDirMaxEntries(ZealFSHeader* header) {
    return getPageSize(header) / sizeof(ZealFileEntry);
}

/**
 * Helper that converts an 8-bit BCD value into a binary value.
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
static inline void freePage(ZealFSHeader* header, uint16_t page) {
    assert(page != 0);
    header->pages_bitmap[page / 8] &= ~(1 << (page % 8));
    header->free_pages++;
}


/**
 * @brief Get the next page of the current from the FAT
 */
static uint16_t getNextFromFat(uint8_t* img, uint16_t current_page)
{
    const ZealFSHeader* header = (ZealFSHeader*) img;
    const int page_size = getPageSize(header);
    /* The FAT starts at page 1 */
    uint16_t* fat = (uint16_t*) (img + page_size);
    return fat[current_page];
}


/**
 * @brief Get the next page of the current from the FAT
 */
static void setNextInFat(uint8_t* img, uint16_t current_page, uint16_t next_page)
{
    const ZealFSHeader* header = (ZealFSHeader*) img;
    const int page_size = getPageSize(header);
    /* The FAT starts at page 1 */
    uint16_t* fat = (uint16_t*) (img + page_size);
    fat[current_page] = next_page;
}


/**
 * @brief Allocate one page in the given header's bitmap.
 *
 * @param header File system header to allocate the page from.
 *
 * @return Page number on success, 0 on error.
 */
static uint16_t allocatePage(ZealFSHeader* header) {
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
        printf("No more space in the bitmap of size: %d\n", header->bitmap_size);
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


/**
 * @brief Get the next power of two of the given number
 */
static inline uint64_t upperPowerOfTwo(long long disk_size)
{
    assert(disk_size != 0);
    int highest_one = 0;
    /* Number of ones in the integer*/
    int ones = 0;

    for (int i = 32; i >= 0; i--) {
        if (disk_size & BIT(i)) {
            if (highest_one == 0) highest_one = i;
            else ones++;
        }
    }

    if (ones == 0) {
        return disk_size;
    }

    /* If we got more than a single 1 bit, we have to return the next power
     * of two, so return BIT(highest_one+1) */
    return BIT(highest_one+1);
}

/**
 * @brief Helper to get the recommended page size from a disk size.
 *
 */
static inline int pageSizeFromDiskSize(long long disk_size)
{
    if (disk_size <= KB(64)) {
        return 256;
    } else if (disk_size <= KB(256)) {
        return 512;
    } else if (disk_size <= MB(1)) {
        return KB(1);
    } else if (disk_size <= MB(4)) {
        return KB(2);
    } else if (disk_size <= MB(16)) {
        return KB(4);
    } else if (disk_size <= MB(64)) {
        return KB(8);
    } else if (disk_size <= MB(256)) {
        return KB(16);
    } else if (disk_size <= GB(1)) {
        return KB(32);
    }

    return KB(64);
}