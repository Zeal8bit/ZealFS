/* SPDX-FileCopyrightText: 2022-2025 Zeal 8-bit Computer <contact@zeal8bit.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define FUSE_USE_VERSION 31

#include <libgen.h>
#include <fuse3/fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>
#include <time.h>
#include <dirent.h>
/* For RENAME_* macros  */
#include <linux/fs.h>
#include "common.h"
#include "zealfs_v2.h"


#define TARGET_TYPE 0x5A    // 'Z'

/* Cache for the image, as the disk image is at most 64KB, we can allocate it from
 * the heap without a problem. */
static uint8_t *g_image;

/**
 * Macro to help converting a page number into an address in the cache.
 */
#define CONTENT_FROM_PAGE(header,page) ( g_image + (((int) (page)) << (8 + (header)->page_size )) )


/**
 * @brief Get the stat structure of an entry in the file system.
 *
 * @param entry Address of the entry to stat.
 * @param st Stat structure to fill.
 */
static void stat_from_entry(ZealFileEntry* entry, struct stat* st)
{
    const uint8_t flags = entry->flags;
    st->st_size = entry->size;
    if (flags & IS_DIR) {
        st->st_nlink = 2;
        st->st_mode = S_IFDIR | 0777;
    } else {
        st->st_nlink = 1;
        st->st_mode = S_IFREG | 0777;
    }

    struct tm tm = { 0 };
    tm.tm_year = (fromBCD(entry->year[0]) * 100 - 1900)
              + fromBCD(entry->year[1]);
    tm.tm_mon = fromBCD(entry->month) - 1;
    tm.tm_mday = fromBCD(entry->day);
    tm.tm_wday = fromBCD(entry->date);
    tm.tm_hour = fromBCD(entry->hours);
    tm.tm_min = fromBCD(entry->minutes);
    tm.tm_sec = fromBCD(entry->seconds);
    time_t t = mktime(&tm);
    st->st_mtime = t;
    st->st_atime = t;
    st->st_ctime = t;
}


/**
 * @brief Function that goes through the absolute path given as a parameter and verifies
 *        that each sub-directory does exist in the disk image.
 *
 * @param path Absolute path in the disk image
 * @param entries Entries array of the current directory. Must be the header's entries when calling
 *                this function.
 * @param root Boolean set to 1 if the given entries is the root directory's entries, 0 else.
 * @param free_entry When not NULL, will be populate with a free entry address in the last directory
 *                   of the path. Useful to create a non-existing-yet file or directory.
 *
 * @return Address of the ZealFileEntry file when found, 0 when the file or directory was not found.
 *         also returns 0 if one of the sub-directories in the path is not existent or is not a directory.
 */
static uint64_t browse_path(const char * path, ZealFileEntry* entries, int root, ZealFileEntry** free_entry)
{
    /* Store the current directory that is followed by '/' */
    char tmp_name[NAME_MAX_LEN + 1] = { 0 };
    ZealFSHeader* header = (ZealFSHeader*) g_image;
    const int max_entries = root ? getRootDirMaxEntries(header) :
                                   getDirMaxEntries(header);

    /* Check the next path '/' */
    char* slash = strchr(path, '/');
    int len = 0;
    if (slash != NULL) {
        len = slash - path;
    } else {
        len = strlen(path);
    }
    if (len > NAME_MAX_LEN) {
        return 0;
    }
    memcpy(tmp_name, path, len);

    for (int i = 0; i < max_entries; i++) {
        if ((entries[i].flags & IS_OCCUPIED) == 0) {
            /* If we are browsing the last name in the path and free_entry is not NULL, we can save this
             * address to return. */
            if (slash == NULL && free_entry) {
                *free_entry = &entries[i];
            }
            continue;
        }
        /* Entry is not empty, check that the name is correct */
        if (strncmp(entries[i].name, tmp_name, NAME_MAX_LEN) == 0) {
            if (slash == NULL) {
                return (uint64_t) &entries[i];
            } else {
                /* Get the page of the current directory */
                return browse_path(slash + 1, (ZealFileEntry*) CONTENT_FROM_PAGE(header, entries[i].start_page), 0, free_entry);
            }
        }
    }

    return 0;
}

/**
 * Small helper to simplify functions that need to fill `info` with a ZealFS entry address
 * before returning.
 */
static int fill_info(struct fuse_file_info * info, uint64_t addr)
{
    info->fh = addr;
    return 0;
}


/**
 * @brief Format the disk image.
 *
 * The file system header, in cache, will be formatted.
 *
 * @param file File descriptor of the opened disk image. It will be truncated to the image size.
 *
 * @return 0 on success, error else
 */
static int format(int file) {
    const int img_fd = common_img_fd();
    const int img_size = common_img_size();

    int err = ftruncate(img_fd, img_size);
    if (err) {
        return err;
    }

    /* Initialize image header */
    ZealFSHeader* header = (ZealFSHeader*) g_image;
    header->magic = TARGET_TYPE;
    /* Version 2 supports extended mode */
    header->version = 2;
    /* According to the size of the disk, we have to calculate the size of the pages */
    const uint16_t page_size_bytes = pageSizeFromDiskSize(img_size);
    /* The page size in the header is the log2(page_bytes/256) - 1 */
    header->page_size = ((sizeof(int) * 8) - (__builtin_clz(page_size_bytes >> 8))) - 1;
    header->bitmap_size = img_size / page_size_bytes / 8;
    /* If the page size is 256, there will be only one page for the FAT */
    const int fat_pages_count = 1 + (header->page_size == 256 ? 0 : 1);
    /* Do not count the first page and the second page */
    header->free_pages = img_size / page_size_bytes - 1 - fat_pages_count;
    /* All the pages are free (0), mark the first one as occupied */
    header->pages_bitmap[0] = 3 | ((fat_pages_count > 1) ? 4 : 0);

    printf("Bitmap size: %d bytes\n", header->bitmap_size);
    printf("Pages size: %d bytes (code %d)\n", page_size_bytes, ((page_size_bytes >> 8) - 1));
    printf("Maximum root entries: %d\n", getRootDirMaxEntries(header));
    printf("Maximum dir entries: %d\n", getDirMaxEntries(header));
    printf("Header size/Root entries: %d (0x%x)\n", getFSHeaderSize(header), getFSHeaderSize(header));

    /* Flush the cache to the file. */
    lseek(img_fd, 0, SEEK_SET);
    write(img_fd, g_image, img_size);

    return 0;
}


/* @brief Check the integrity of the image file loaded in `image` variable
 *
 * @return 0 on success, 1 on error
 */
static int check_integrity(void)
{
    ZealFSHeader* header = (ZealFSHeader*) g_image;
    /* Size of the file according to the bitmap */
    const int image_size = header->bitmap_size * 8 * getPageSize(header);
    const int requested_size = common_img_size();

    if (header->magic != 'Z') {
        printf("Error: invalid magic header in the image. Corrupted file?\n");
        return 1;
    }

    if (header->bitmap_size == 0) {
        printf("Error: invalid 0 size for bitmap. Corrupted file?\n");
        return 1;
    }

    if (image_size > requested_size) {
        printf("Error: invalid bitmap size. Header says the image is %d bytes (%d bytes/page) but actual file size is %d\n",
                image_size, getPageSize(header), requested_size);
        return 1;
    }

    if (image_size < requested_size) {
        printf("Warning: image size according to the bitmap is smaller than file size, "
               "some part of the image will be unreachable.\n");
    }

    /* Count the number of free pages in the bitmap compared to the field in the header */
    int count = 0;
    for (int i = 0; i < header->bitmap_size; i++) {
        uint8_t map = header->pages_bitmap[i];
        /* Count the numbers of 1 and subtract it to 8 */
        count += 8 - __builtin_popcount(map);
    }

    if (count < header->free_pages) {
        printf("Warning: the number of pages marked free is smaller than the actual count, "
               "some pages may be unreachable.\n");
    }

    if (count > header->free_pages) {
        printf("Error: the number of pages marked free is bigger than the actual count. Corrupted file?\n");
        return 1;
    }

    return 0;
}


/**
 * @brief Initialize the FUSE subsystem with our file system.
 */
static void *zealfs_init(struct fuse_conn_info *conn,
            struct fuse_config *cfg)
{
    (void) conn;
    cfg->kernel_cache = 1;
    return NULL;
}

/**
 * @brief Get the attributes of a file or directory. (path)
 *        Underneath, this function will call `stat_from_entry`.
*/
static int zealfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
    (void) fi;

    memset(stbuf, 0, sizeof(struct stat));
    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        /* Directory size is a page size */
        stbuf->st_size = 256;
        return 0;
    }

    /* Not '/' */
    ZealFSHeader* header = (ZealFSHeader*) g_image;

    uint64_t index = browse_path(path + 1, getRootDirEntries(header), 1, NULL);
    ZealFileEntry* entry = (ZealFileEntry*) index;
    if (index > 0) {
        stat_from_entry(entry, stbuf);
        return 0;
    }

    return -ENOENT;
}

/**
 * @brief Open the file or directory given as a parameter.
 */
static int zealfs_open(const char *path, struct fuse_file_info *info)
{
    ZealFSHeader* header = (ZealFSHeader*) g_image;

    if (strcmp(path, "/") == 0) {
        return -EISDIR;
    }

    uint64_t index = browse_path(path + 1, getRootDirEntries(header), 1, NULL);
    ZealFileEntry* entry = (ZealFileEntry*) index;
    if (entry) {
        if (entry->flags & 1) {
            return -ENOTDIR;
        }
        return fill_info(info, index);
    }
    return -ENOENT;
}


/**
 * @brief Remove a file (and only a file!) from the disk image.
 */
static int zealfs_unlink(const char* path)
{
    ZealFSHeader* header = (ZealFSHeader*) g_image;

    uint64_t index = browse_path(path + 1, getRootDirEntries(header), 1, NULL);
    ZealFileEntry* entry = (ZealFileEntry*) index;
    if (index == 0) {
        return -ENOENT;
    }
    if (entry->flags & IS_DIR) {
        return -EISDIR;
    }

    uint16_t page = entry->start_page;
    while (page != 0) {
        freePage(header, page);
        const uint16_t next = getNextFromFat(g_image, page);
        setNextInFat(g_image, page, 0);
        page = next;
    }
    /* Clear the flags of the file entry */
    entry->flags = 0;

    return 0;
}


/**
 * @brief Rename an entry, file or directory, in the disk image.
 *        The content will not be altered nor modified, only the entries headers will.
 */
static int zealfs_rename(const char* from, const char* to, unsigned int flags)
{
    ZealFSHeader* header = (ZealFSHeader*) g_image;
    ZealFileEntry* free_entry = NULL;
    ZealFileEntry* fentry = (ZealFileEntry*) browse_path(from + 1, getRootDirEntries(header), 1, NULL);
    ZealFileEntry* tentry = (ZealFileEntry*) browse_path(to + 1,   getRootDirEntries(header), 1, &free_entry);

    if (fentry == 0 || (tentry == 0 && flags == RENAME_EXCHANGE)) {
        return -ENOENT;
    }

    if (flags == RENAME_NOREPLACE && tentry) {
        return -EEXIST;
    }

    if (flags == RENAME_EXCHANGE) {
        return -EFAULT;
    }

    char* from_mod = strdup(from);
    const char* from_dir = dirname(from_mod);
    char* to_mod = strdup(to);
    const char* to_dir = dirname(to_mod);
    char* to_mod_name = strdup(to);
    const char* newname = basename(to_mod_name);

    /* Check if the new name is valid */
    const int len = strlen(newname);
    if (len > NAME_MAX_LEN)
    {
        free(to_mod);
        free(from_mod);
        free(to_mod_name);
        return -ENAMETOOLONG;
    }

    /* In all cases, if the destination file already exists, remove it! */
    if (tentry) {
        zealfs_unlink(to);
        free_entry = tentry;
    }
    /* And rename the source file in its own directory */
    memset(fentry->name, 0, NAME_MAX_LEN);
    strncpy(fentry->name, newname, NAME_MAX_LEN);

    /* Check if the source and destination are in the same directory */
    int same_dir = strcmp(from_dir, to_dir) == 0;
    free(to_mod);
    free(from_mod);
    free(to_mod_name);

    if (!same_dir) {
        /* Not in the same directory, move the header if we have a free entry */
        if (!free_entry) {
            return -ENOMEM;
        }
        memcpy(free_entry, fentry, sizeof(ZealFileEntry));
        /* Mark the former one as empty */
        memset(fentry, 0, sizeof(ZealFileEntry));
    }

    return 0;
}


/**
 * @brief Remove an empty directory from the disk image.
 */
static int zealfs_rmdir(const char* path)
{
    ZealFSHeader* header = (ZealFSHeader*) g_image;

    if (strcmp(path, "/") == 0) {
        return -EACCES;
    }

    uint64_t index = browse_path(path + 1, getRootDirEntries(header), 1, NULL);
    ZealFileEntry* entry = (ZealFileEntry*) index;
    if (index == 0) {
        return -ENOENT;
    }
    if ((entry->flags & IS_DIR) == 0) {
        return -ENOTDIR;
    }

    /* Check that the directory is empty */
    uint16_t page = entry->start_page;
    ZealFileEntry* entries = (ZealFileEntry*) CONTENT_FROM_PAGE(header, page);

    const int max_entries = getDirMaxEntries(header);
    for (int i = 0; i < max_entries; i++) {
        if (entries[i].flags & IS_OCCUPIED) {
            return -ENOTEMPTY;
        }
    }
    /* Clear the flags of the entry */
    entry->flags = 0;

    return 0;
}


/**
 * @brief Private function used to create either a directory of a file in the disk image.
 *
 * @param isdir 1 to create a directory, 0 to create a file.
 * @param path Absolute path of the entry to create.
 * @param mode Unused.
 * @param info When not NULL, filled with the newly allocated ZealFS Entry address.
 *
 * @return 0 on success, error else.
 */
static int zealfs_create_both(int isdir, const char * path, mode_t mode, struct fuse_file_info *info)
{
    ZealFSHeader* header = (ZealFSHeader*) g_image;
    ZealFileEntry* empty = NULL;

    uint64_t index = browse_path(path + 1, getRootDirEntries(header), 1, &empty);
    if (index) {
        return -EEXIST;
    }
    if (!empty) {
        return -ENFILE;
    }

    char* path_mod = strdup(path);
    const char* filename = basename(path_mod);
    const int len = strlen(filename);

    if (len > NAME_MAX_LEN) {
        free(path_mod);
        return -ENAMETOOLONG;
    }

    if (info) {
        info->fh = (uint64_t) empty;
    }

    /* Populate the entry */
    unsigned int offset = (void*) empty - (void*) g_image;
    uint8_t newp = allocatePage((ZealFSHeader*) g_image);
    printf("%s: allocating %s at page 0x%x (%d)."
           "Entry offset in disk: %x (%d)\n",
            path, isdir ? "dir": "file", newp, newp,
            offset, offset);
    if (newp == 0) {
        free(path_mod);
        return -EFBIG;
    }
    empty->flags = IS_OCCUPIED | isdir;
    empty->start_page = newp;
    memset(&empty->name, 0, 16);
    memcpy(&empty->name, filename, len);
    empty->size = isdir ? 256 : 0;
    /* Set the date in the structure */
    time_t rawtime;
    time(&rawtime);
    struct tm* timest = localtime(&rawtime);
    /* Got the time, populate it in the structure */
    empty->year[0] = toBCD((1900 + timest->tm_year) / 100);   /* 20 first */
    empty->year[1] = toBCD(timest->tm_year);         /* 22 then */
    empty->month = toBCD(timest->tm_mon + 1);
    empty->day = toBCD(timest->tm_mday);
    empty->date = toBCD(timest->tm_wday);
    empty->hours = toBCD(timest->tm_hour);
    empty->minutes = toBCD(timest->tm_min);
    empty->seconds = toBCD(timest->tm_sec);

    /* Empty the page */
    uint8_t* content = CONTENT_FROM_PAGE(header, newp);
    printf("\tPage offset for the dir: 0x%lx (%ld)\n", content - g_image, content - g_image);
    memset(content, 0, 256);

    free(path_mod);
    return 0;
}


/**
 * @brief Create an empty file in the disk image.
 *
 * @note Underneath, this function calls `zealfs_create_both`
 */
static int zealfs_create(const char * path, mode_t mode, struct fuse_file_info *info)
{
    return zealfs_create_both(0, path, mode, info);
}


/**
 * @brief Create an empty directory in the disk image.
 *
 * @note Underneath, this function calls `zealfs_create_both`
 */
static int zealfs_mkdir(const char * path, mode_t mode)
{
    return zealfs_create_both(1, path, mode, NULL);
}


/**
 * @brief Read data from an opened file.
 *
 * @param path Path of the file to read. (unused)
 * @param buf Buffer to fill with file's data.
 * @param size Size of the buffer.
 * @param offset Offset in the file to start reading from.
 * @param fi File info containing the ZealFS Entry address of the opened file.
 *
 * @return number of bytes read from the file.
 */
static int zealfs_read(const char *path, char *buf, size_t size, off_t offset,
              struct fuse_file_info *fi)
{
    ZealFSHeader* header = (ZealFSHeader*) g_image;
    ZealFileEntry* entry = (ZealFileEntry*) fi->fh;
    /* Subtract 2 for the pointer to the next page */
    const int data_bytes_per_page = getPageSize(header);
    int jump_pages = offset / data_bytes_per_page;
    int offset_in_page = offset % data_bytes_per_page;

    size = MIN(size, entry->size);
    const int total = size;

    uint16_t current_page = entry->start_page;
    while (jump_pages) {
        current_page = getNextFromFat(g_image, current_page);
        jump_pages--;
    }

    uint8_t* page = CONTENT_FROM_PAGE(header, current_page);

    while (size) {
        int count = MIN(data_bytes_per_page - offset_in_page, size);
        memcpy(buf, page + offset_in_page, count);
        buf += count;
        if (size != count) {
            current_page = getNextFromFat(g_image, current_page);
            page = CONTENT_FROM_PAGE(header, current_page);
        }
        size -= count;
        offset_in_page = 0;
    }

    return total;
}


/**
 * @brief Write data to an opened file.
 *
 * @param path Path of the file to write. (unused)
 * @param buf Buffer containing the data to write to file.
 * @param size Size of the buffer.
 * @param offset Offset in the file to start writing from.
 * @param fi File info containing the ZealFS Entry address of the opened file.
 *
 * @return number of bytes written to the file, -EFBIG if the size is too big.
 */
static int zealfs_write(const char *path, const char *buf, size_t size, off_t offset,
              struct fuse_file_info *fi)
{
    ZealFSHeader* header = (ZealFSHeader*) g_image;
    ZealFileEntry* entry = (ZealFileEntry*) fi->fh;
    /* Subtract 2 for the pointer to the next page */
    const int data_bytes_per_page = getPageSize(header);
    int jump_pages = offset / data_bytes_per_page;
    int offset_in_page = offset % data_bytes_per_page;
    const int remaining_in_page = data_bytes_per_page - offset_in_page;

    const int total = size;

    /* Check if we have enough pages. */
    if (header->free_pages * data_bytes_per_page + remaining_in_page < size) {
        return -EFBIG;
    }

    uint8_t* page = CONTENT_FROM_PAGE(header, entry->start_page);
    while (jump_pages) {
        page = CONTENT_FROM_PAGE(header, *page);
        jump_pages--;
    }

    uint16_t current_page = entry->start_page;

    while (size) {
        int count = MIN(data_bytes_per_page - offset_in_page, size);
        // printf("Writing: %d, remaining %ld\n", count, size);
        memcpy(page + offset_in_page, buf, count);
        entry->size += (uint16_t) count;
        buf += count;
        size -= count;

        /* In all cases, check the next page */
        uint16_t next = getNextFromFat(g_image, current_page);
        if (next) {
            current_page = next;
        } else if (size) {
            /* Only allocate a new page if we still need to write some bytes */
            next = allocatePage((ZealFSHeader*) g_image);
            if (next == 0) {
                return -ENOSPC;
            }

            /* Link the newly allocated page to the current page */
            setNextInFat(g_image, current_page, next);

            /* Make it the current page */
            current_page = next;

            page = CONTENT_FROM_PAGE(header, current_page);
        }

        offset_in_page = 0;
    }

    return total;
}


/**
 * @brief Open a directory from the disk image.
 *
 * @param path Absolute path to the directory to open.
 * @param info Info structure that will be filled with the ZealFileEntry address.
 *
 * @return 0 on success, error code else.
 */
static int zealfs_opendir(const char * path, struct fuse_file_info * info)
{
    ZealFSHeader* header = (ZealFSHeader*) g_image;

    if (strcmp(path, "/") == 0) {
        return fill_info(info, (uint64_t) getRootDirEntries(header));
    }

    uint64_t index = browse_path(path + 1, getRootDirEntries(header), 1, NULL);
    ZealFileEntry* entry = (ZealFileEntry*) index;
    if (entry) {
        if (entry->flags & 1) {
            return fill_info(info, (uint64_t) CONTENT_FROM_PAGE(header, entry->start_page));
        }
        return -ENOTDIR;
    }
    return -ENOENT;
}


/**
 * @brief Read all entries from an opened directory.
 *
 * @param path Path of the directory to browse. (unused)
 * @param buf Context given by FUSE when adding new entries.
 * @param filler Function to call to add an entry in the directory.
 * @param offset Unused.
 * @param info Info structure that will be filled with the opened ZealFileEntry address.
 * @param flags Unused.
 */
static int zealfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
             off_t offset, struct fuse_file_info *info,
             enum fuse_readdir_flags flags)
{
    (void) offset;
    (void) flags;

    ZealFSHeader* header = (ZealFSHeader*) g_image;
    char name[NAME_MAX_LEN + 1] = { 0 };

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    ZealFileEntry* entries = (ZealFileEntry*) info->fh;
    /* If the directory we are browsing is the root directory, we have less entries */
    const int max_entries = (entries == getRootDirEntries(header)) ?
                                getRootDirMaxEntries(header) :
                                getDirMaxEntries(header);

    /* Browse each entry, looking for an non-empty one thanks to the flags */
    for (int i = 0; i < max_entries; i++) {
        const uint8_t flags = entries[i].flags;
        if (flags & IS_OCCUPIED) {
            struct stat st = { 0 };
            stat_from_entry(&entries[i], &st);
            strncpy(name, entries[i].name, NAME_MAX_LEN);
            filler(buf, name, &st, 0, 0);
        }
    }

    return 0;
}


/**
 * @brief Called when the image in unmounted.
 *
 * This function will flush the cache (disk image) into the file.
 */
static void zealfs_destroy(void *private_data)
{
    /* Flush cached data to file */
    int fd = common_img_fd();
    int offset = common_img_offset();
    lseek(fd, offset, SEEK_SET);
    write(fd, g_image, common_img_size());
    close(fd);
}


/**
 * @brief Look for a ZealFS partition in the image file. If it has an MBR, it will check the 4 partitions inside,
 * If it's a raw image, it will return sector 0 and the image size.
 */
#define MBR_SIZE                512
#define PARTITION_ENTRY_SIZE    16
#define PARTITION_TABLE_OFFSET  446
#define PARTITION_TYPE_OFFSET   4
#define LBA_OFFSET              8
#define SECTOR_COUNT_OFFSET     12
#define SECTOR_SIZE             512

int mbr_find_partition(const char* filename, int filesize, off_t *offset, int* size)
{
    uint8_t mbr[MBR_SIZE];

    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        perror("Could not open file");
        return 0;
    }

    ssize_t bytes_read = read(fd, mbr, MBR_SIZE);
    if (bytes_read != MBR_SIZE) {
        perror("Failed to read MBR");
        close(fd);
        return 0;
    }

    /* Check for MBR signature (0x55AA at offset 510) */
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        /* "Invalid MBR signature, check if it's a raw ZealFS image */
        close(fd);
        if (mbr[0] == TARGET_TYPE) {
            *offset = 0;
            *size = filesize;
            return 1;
        }
        return 0;
    }

    /* Iterate through the 4 partition entries */
    for (int i = 0; i < 4; i++) {
        uint8_t *entry = mbr + PARTITION_TABLE_OFFSET + i * PARTITION_ENTRY_SIZE;
        uint8_t type = entry[PARTITION_TYPE_OFFSET];

        if (type == TARGET_TYPE) {
            const uint32_t lba = *(uint32_t *)(entry + LBA_OFFSET);
            const uint32_t sector_count = *(uint32_t *)(entry + SECTOR_COUNT_OFFSET);
            *offset = (off_t)lba * SECTOR_SIZE;
            printf("Sector count: %d, sector size: %d\n", sector_count, SECTOR_SIZE);
            *size = sector_count * SECTOR_SIZE;
            close(fd);
            return 1;
        }
    }
    close(fd);
    return 0;
}


/**
 * @brief Image init, first function called, right after parsing the command line options
 */
static int zealfs_image_init(zealfs_context* ctx)
{
    /* Check if the file is already existing */
    struct stat st = { 0 };
    int trunc = 0;
    if (stat(ctx->img_file, &st) != 0) {
        /* File doesn't exist, we need to truncate the new file */
        trunc = 1;
    } else if (mbr_find_partition(ctx->img_file, st.st_size, &ctx->offset, &ctx->size)) {
        printf("Found ZealFS partition at offset 0x%lx, size %d bytes\n", ctx->offset, ctx->size);
    } else {
        printf("Could not find any ZealFS partition in the existing image\n");
        return 1;
    }

    /* Create a cache for the file */
    g_image = calloc(1, ctx->size);
    if (g_image == NULL) {
        printf("Could not allocate enough memory!\n");
        return 1;
    }

    ctx->img_fd = open(ctx->img_file, O_RDWR | O_CREAT, 0644);
    if (ctx->img_fd < 0) {
        perror("Could not open image file");
        return 2;
    }

    if (trunc && format(ctx->img_fd)) {
        perror("Could not set new file size");
        return 3;
    }

    if (!trunc) {
        /* Seek to the ZealFS partition */
        assert(lseek(ctx->img_fd, ctx->offset, SEEK_SET) == ctx->offset);
        int rd = read(ctx->img_fd, g_image, ctx->size);
        assert(rd == ctx->size);
    }

    /* Check the integrity of the image */
    if (check_integrity()) {
        return 4;
    }

    return 0;
}


/**
 * @brief FUSE operations associated to our file system.
 */
const zealfs_operations zealfs_v2_ops = {
    .fuse_ops = {
        .init     = zealfs_init,
        .getattr  = zealfs_getattr,
        .opendir  = zealfs_opendir,
        .readdir  = zealfs_readdir,
        .open     = zealfs_open,
        .read     = zealfs_read,
        .create   = zealfs_create,
        .write    = zealfs_write,
        .unlink   = zealfs_unlink,
        .rename   = zealfs_rename,
        .mkdir    = zealfs_mkdir,
        .rmdir    = zealfs_rmdir,
        .destroy  = zealfs_destroy,
    },
    .image_init = zealfs_image_init,
};
