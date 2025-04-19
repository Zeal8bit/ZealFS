/* SPDX-FileCopyrightText: 2025 Zeal 8-bit Computer <contact@zeal8bit.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once


/* Options used to parse the parameters given from the command line. */
typedef struct {
    const char *img_file;
    int size;
    int show_help;
    int img_fd;
} zealfs_context;


typedef struct zealfs_operations {
    struct fuse_operations fuse_ops;
    int (*image_init)(zealfs_context*);
} zealfs_operations;


int common_img_fd(void);

int common_img_size(void);
