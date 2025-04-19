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
#include "zealfs_v1.h"


/* Cache for the image, as the disk image is at most 64KB, we can allocate it from
 * the heap without a problem. */
static uint8_t *g_image;


/**
 * Macro to help converting a page number into an address in the cache.
 */
#define CONTENT_FROM_PAGE(page) ( g_image + (((int) page) << 8) )


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
    const int max_entries = root ? ROOT_MAX_ENTRIES : DIR_MAX_ENTRIES;

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
                int page = entries[i].start_page << 8;
                return browse_path(slash + 1, (ZealFileEntry*) (g_image + page), 0, free_entry);
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
    header->magic = 'Z';
    header->version = 1;
    /* Do not count the first page */
    header->bitmap_size = img_size / 256 / 8;
    /* Do not count the first page */
    header->free_pages = img_size / 256 - 1;
    /* All the pages are free (0), mark the first one as occupied */
    header->pages_bitmap[0] = 1;
    memset(header->reserved, 0, sizeof(header->reserved));

    /* Flush the cache to the file. */
    lseek(img_fd, 0, SEEK_SET);
    write(img_fd, g_image, img_size);

    return 0;
}


/* @brief Check the integrity of the image file loaded in `image` variable
 *
 * @return 0 on success, 1 on error
 */
int check_integrity()
{
    ZealFSHeader* header = (ZealFSHeader*) g_image;
    /* Size of the file according to the bitmap */
    const int image_size = header->bitmap_size * 8 * 256;
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
        printf("Error: invalid bitmap size. Header says the image is %d bytes but actual file size is %d\n",
                image_size, requested_size);
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

    uint64_t index = browse_path(path + 1, header->entries, 1, NULL);
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

    uint64_t index = browse_path(path + 1, header->entries, 1, NULL);
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

    uint64_t index = browse_path(path + 1, header->entries, 1, NULL);
    ZealFileEntry* entry = (ZealFileEntry*) index;
    if (index == 0) {
        return -ENOENT;
    }
    if (entry->flags & IS_DIR) {
        return -EISDIR;
    }

    uint8_t page = entry->start_page;
    while (page != 0) {
        freePage(header, page);
        page = g_image[page << 8];
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
    ZealFileEntry* fentry = (ZealFileEntry*) browse_path(from + 1, header->entries, 1, NULL);
    ZealFileEntry* tentry = (ZealFileEntry*) browse_path(to + 1, header->entries, 1, &free_entry);

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

    uint64_t index = browse_path(path + 1, header->entries, 1, NULL);
    ZealFileEntry* entry = (ZealFileEntry*) index;
    if (index == 0) {
        return -ENOENT;
    }
    if ((entry->flags & IS_DIR) == 0) {
        return -ENOTDIR;
    }

    /* Check that the directory is empty */
    uint8_t page = entry->start_page;
    ZealFileEntry* entries = (ZealFileEntry*) CONTENT_FROM_PAGE(page);

    for (int i = 0; i < DIR_MAX_ENTRIES; i++) {
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

    uint64_t index = browse_path(path + 1, header->entries, 1, &empty);
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
    uint8_t newp = allocatePage((ZealFSHeader*) g_image);
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
    uint8_t* content = CONTENT_FROM_PAGE(newp);
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
    ZealFileEntry* entry = (ZealFileEntry*) fi->fh;
    int jump_pages = offset / 255;
    int offset_in_page = offset % 255;

    size = MIN(size, entry->size);
    const int total = size;

    uint8_t* page = CONTENT_FROM_PAGE(entry->start_page);
    while (jump_pages) {
        page = CONTENT_FROM_PAGE(*page);
        jump_pages--;
    }

    while (size) {
        int count = MIN(255 - offset_in_page, size);
        memcpy(buf, page + 1 + offset_in_page, count);
        buf += count;
        if (size != count) {
            page = CONTENT_FROM_PAGE(*page);
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
    int jump_pages = offset / 255;
    int offset_in_page = offset % 255;
    const int remaining_in_page = 255 - offset_in_page;

    const int total = size;

    /* Check if we have enough pages. */
    if (header->free_pages * 255 + remaining_in_page < size) {
        return -EFBIG;
    }

    uint8_t* page = CONTENT_FROM_PAGE(entry->start_page);
    while (jump_pages) {
        page = CONTENT_FROM_PAGE(*page);
        jump_pages--;
    }

    while (size) {
        int count = MIN(255 - offset_in_page, size);
        // printf("Writing: %d, remaining %ld\n", count, size);
        memcpy(page + 1 + offset_in_page, buf, count);
        entry->size += (uint16_t) count;
        buf += count;
        size -= count;

        /* In all cases, check the next page */
        uint8_t next = *page;
        if (next) {
            page = CONTENT_FROM_PAGE(*page);
        } else if (size) {
            /* Only allocate a new page if we still need to write some bytes */
            next = allocatePage((ZealFSHeader*) g_image);
            assert( next != 0 );
            *page = next;
            page = CONTENT_FROM_PAGE(next);
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
        return fill_info(info, (uint64_t) &header->entries);
    }

    uint64_t index = browse_path(path + 1, header->entries, 1, NULL);
    ZealFileEntry* entry = (ZealFileEntry*) index;
    if (entry) {
        if (entry->flags & 1) {
            return fill_info(info, (uint64_t) CONTENT_FROM_PAGE(entry->start_page));
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
    const int max_entries = (entries == header->entries) ? ROOT_MAX_ENTRIES : DIR_MAX_ENTRIES;

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
    lseek(fd, 0, SEEK_SET);
    write(fd, g_image, common_img_size());
    close(fd);
}


/**
 * @brief Image init, first function called, right after parsing the command line options
 */
static int zealfs_image_init(zealfs_context* ctx)
{
    /* Convert the size to bytes and check that it's valid */
    if (ctx->size > 64) {
        printf("Invalid size %d\n"
               "Provided size must be less or equal to 64KB\n",
               ctx->size);
        return 1;
    }
    ctx->size *= 1024;

    /* Check if the file already exists */
    struct stat st = { 0 };
    int trunc = 0;
    if (stat(ctx->img_file, &st) != 0) {
        /* File doesn't exist, we need to truncate the new file */
        trunc = 1;
    } else {
        ctx->size = st.st_size;
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
        int rd = read(ctx->img_fd, g_image, ctx->size);
        if (rd != ctx->size) {
            printf("Could not read the image file!\n");
            return 1;
        }
    }

    /* Check the integrity of the image */
    if (check_integrity()) {
        return 4;
    }

    return 0;
}


/**
 * @brief FUSE operations associated to our file system.
 *
 * TODO: Implement truncate function.
 */
const zealfs_operations zealfs_v1_ops = {
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
