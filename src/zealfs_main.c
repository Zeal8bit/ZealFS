/* SPDX-FileCopyrightText: 2022-2025 Zeal 8-bit Computer <contact@zeal8bit.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define FUSE_USE_VERSION 31

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <libgen.h>
#include <fuse3/fuse_opt.h>
#include <fuse3/fuse.h>
#include "common.h"


/* The default name for the disk image can be provided from the Makefile or command
 * line. If it was not provided, define it here.
 */
#ifndef DEFAULT_IMAGE_NAME
#define DEFAULT_IMAGE_NAME "zfs.img"
#endif

#ifndef DEFAULT_IMAGE_SIZE_KB
#define DEFAULT_IMAGE_SIZE_KB 32
#endif

/* Declare the FUSE structures for all the supported version of ZealFS */
extern zealfs_operations zealfs_v1_ops;
extern zealfs_operations zealfs_v2_ops;

static zealfs_context options;

#define OPTION(t, p)                           \
    { t, offsetof(zealfs_context, p), 1 }

static const struct fuse_opt option_spec[] = {
    OPTION("--image=%s", img_file),
    OPTION("--size=%d", size),
    OPTION("--mbr", mbr),
    OPTION("-v1", v1),
    OPTION("-v2", v2),
    OPTION("-h", show_help),
    OPTION("--help", show_help),
    FUSE_OPT_END
};

/**
 * @brief Show the help with the possible options
 */
static void show_help(const char *program)
{
    printf("usage: %s [options] <mountpoint>\n\n", program);
    printf("File-system specific options:\n"
            "    --image=<s>          Name of the image file, \"" DEFAULT_IMAGE_NAME "\" by default\n"
            "    --size=<s>           Size of the new image file in KB if not existing\n"
            "    --mbr                Create an MBR in the new image file if not existing (ZealFSv2 only)\n"
            "    -v1                  Use ZealFSv1 (64KB limit) for the given image\n"
            "    -v2                  Use ZealFSv2 (4GB limit) for the given image\n"
            "\n");
}


int common_img_fd(void)
{
    return options.img_fd;
}

int common_img_size(void)
{
    return options.size;
}

int common_img_mbr(void)
{
    return options.mbr;
}

off_t common_img_offset(void)
{
    return options.offset;
}

int main(int argc, char *argv[])
{
    int ret;
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    options.size = DEFAULT_IMAGE_SIZE_KB;

    /* Parse options */
    if (fuse_opt_parse(&args, &options, option_spec, NULL) == -1)
        return 1;

    /* When --help is specified, first print our own file-system
       specific help text, then signal fuse_main to show
       additional help (by adding `--help` to the options again)
       without usage: line (by setting argv[0] to the empty
       string) */
    if (options.img_file == NULL || options.show_help) {
        show_help(argv[0]);
        printf("FUSE options:\n");
        fuse_lib_help(&args);
        args.argv[0][0] = '\0';
        return 1;
    }

    printf("Info: using disk image %s\n", options.img_file);
    /* Size is given in KB */
    options.size *= 1024;

    /* Call the implementation init function, it returns 0 on success, non-zero value else.
     * Make sure only one version was provided */
    zealfs_operations* ops = NULL;
    if (options.v1 && options.v2) {
        printf("ERROR: Invalid ZealFS version!\nPlease provide a single version\n");
        show_help(argv[0]);
        return 1;
    } else if (options.v1) {
        ops = &zealfs_v1_ops;
        if (options.mbr) {
            printf("WARNING: MBR creation not valid with ZealFSv1, ignoring\n");
        }
    } else if (options.v2) {
        ops = &zealfs_v2_ops;
    } else {
        printf("ERROR: Please specify a ZealFS version\n");
        show_help(argv[0]);
        return 1;
    }

    ret = ops->image_init(&options);
    if (ret != 0) {
        return ret;
    }

    ret = fuse_main(args.argc, args.argv, &ops->fuse_ops, NULL);
    fuse_opt_free_args(&args);
    return ret;
}
