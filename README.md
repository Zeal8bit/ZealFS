<p align="center">
    <img src="img/title.jpg" alt="ZealFS title" />
</p>
<p align="center">
    <a href="https://opensource.org/licenses/Apache-2.0">
        <img src="https://img.shields.io/badge/License-Apache_2.0-blue.svg" alt="Licence" />
    </a>
    <p align="center">A file system made for small storage and implementable on 8-bit computers!</p>
</p>

## Yet another file system?

If you follow my work, you may be aware that I am developing a Z80-based 8-bit computer entirely from scratch: from PCB to software. It integrates an I2C EEPROM socket that can accommodate any compatible chip, usually ranging from 2KB to 128KB. As such, they are smaller than floppy disks or other flash memories. Moreover, the operating system running on it, Zeal 8-bit OS, is based around disks, files and directories, I was looking for a file system with a low storage overhead and simple enough to be implemented in Z80 assembly.

That excluded all FAT12, FAT16, as well as other modern alternatives (FAT32, exFAT, NTFS, Ext2/3/4, etc...). Other custom smaller FAT alternatives I could find online had their own limitations, such as the absence of directories.

This is why ZealFS v1 was created at first!

After a while, Zeal 8-bit Computer supported new storages devices such as Compact Flah cards and TF cards (microSD). Their sizes are in the range of MB and GB respectively, making ZealFS v1 too restricted. Therefore, a new version of ZealFS, ZealFS v2, was implemented, it fixes some designs issues of the first version while still providing very low overhead for small memories storages!

As the Zeal 8-bit Computer evolved, it began supporting additional storage devices such as CompactFlash and TF (microSD) cards. With storage capacities ranging from megabytes to gigabytes, ZealFSv1 became too limited. To address this, a new version — ZealFS v2 — was developed. It resolves several design limitations of the original while maintaining minimal overhead, especially for use with small memory devices.

> Please note that ZealFS v2 is **not** backward compatible with ZealFS v1, but the implemented FUSE driver supports both versions

## Features

### ZealFS v1

Here is a non-exhaustive list of the current features for ZealFS v1:

* Supports files and directories with unlimited nesting depth
* Handles up to 64KB total storage
* File and directory names up to 16 characters
* Timestamps stored in BCD format
* No fixed file size limit — only constrained by available storage
* Minimal overhead thanks to 256-byte pages, suitable for small-memory systems
* Supports up to 8 entries per directory (_root directory is limited to 6 entries_)
* Compact implementation:
    * ~1.3 KB of Z80 assembly (for the Zeal 8-bit OS)
    * <1000 lines of C (FUSE version)

### ZealFS v2

Here is a non-exhaustive list of the current features for ZealFS v2:

* Supports files and directories with unlimited nesting depth
* Handles up to 4 GB total storage, with page sizes from 256 bytes to 64 KB (up to 65,536 pages)
* File and directory names up to 16 characters
* Timestamps stored in BCD format
* No fixed file size limit — only constrained by available storage
* FAT table for fast next-page lookups
* Minimal overhead, suitable for small-memory and large-memory systems
* No limit on the number of entries per directory
* Compact implementation:
    * ~2 KB of Z80 assembly (for the Zeal 8-bit OS)
    * <1000 lines of C (FUSE version)


## Compilation and usage

This repository contains an implementation of ZealFS for Linux, in the form of a FUSE implementation. [More info about FUSE on their project page](https://github.com/libfuse/libfuse). It was tested on Ubuntu 22.04 LTS.

### Dependencies

In order to use the example you will need:

* make
* libfuse3-dev
* gcc (or any other compatible C compiler)

For example, on Ubuntu, you can install the main dependencies with:

```
sudo apt install libfuse3-dev make
```

if you're on debian, run:

```
sudo apt install fuse3 libfuse3-dev make pkg-config
```

### Compilation

To compile the project, type:

```
make
```

Upon success, a new binary is created named `zealfs` by default.

### Usage

In order to use the file system, you will need a disk image and a mount point. If you don't have any, no worries, the binary will create one. Use the options `--image` and `--size` to choose the destination image and the image size, in KB, respectively. You must also specify the version of ZealFS to use with `-v1` or `-v2`. For example, you can use:

```
./zealfs --image=my_disk.img --size=64 -v1 my_mount_dir
```

This will create a new disk image, that uses ZealFS v1, named `my_disk.img` of size 64KB and mounted in the directory `my_mount_dir` present in the current folder. The binary should show:

```
$ mkdir my_mount_dir
$ ./zealfs --image=my_disk.img --size=64 my_mount_dir
Info: using disk image my_disk.img
$
```

The binary is then running in background, the content of the disk image can be populated or checked either through the terminal or with a GUI file explorer, just like regular file systems.

You can get all the possible parameters by using command:

```
./zealfs --help
```

### Unmounting the disk image

After using the disk image, you must unmount it thanks to the command:

```
umount my_mount_dir
```

**Unmounting is very important, as it will flush all the data written to the virtual disk into the actual image file!**

# Implementation details

## ZealFS v1

### Pages

The file system is based around virtual pages. No matter how big the storage is, it will be virtually split into 256-byte pages. As such, a memory of size `N`KB will have `N*1024/256` virtual pages.

A page is always designated by its index, which is also called "number" in the code. So page index/number 0 designates the page that starts at address `0x0000`, page 1 designates the one starting at address `0x0100`, etc...
We can also say that the page number/index represents the upper 8-bit of the page address in the storage.

Pages are the smallest entities in the file system that can be allocated and freed. Thus, the smallest entities (files) are 256-byte big.

The following diagram shows an example of the virtual pages on a 64KB disk:
![File System Pages](img/pages.jpg)

### Header

The first page of the file system is reserved and **always** allocated: it's the header.

It contains the following information:

* **Magic byte** — 1 byte, ASCII 'Z' (0x5A)
* **Version** — 1 byte, implementation version, must be 1 for ZealFS v1
* **Bitmap size** — 1 byte
* **Free page count** — 1 byte indicating the number of available pages
* **Page allocation bitmap** — always 32 bytes (see details below)
* **Reserved area** — 28 bytes
* **Root directory entries**

The first value, magic byte is used to recognize easily if the file is a ZealFS disk image or not.

The second byte, represents the version of ZealFS the current image/disk is implementing.

The third field, as is name states, represents the size of the bitmap in **bytes**.

The fourth field represents the number of free pages in the disks, this value may be redundant with the bitmap, but keep in mind that this file system was designed with 8-bit implementation in mind, so it is easier and faster to check this value than browsing the whole bitmap.

The fifth field is the bitmap of allocated pages, always 32 bytes, even on memories smaller than 64KB. The bitmap is aligned on 32-bit, which makes it possible for a (little-endian) host computers to cast this array into a 32-bit one, for faster allocate and free operations.

The following bytes are currently unused but reserved for future use. They also serve as a padding for the next entry.

Finally, with the remaining space in the page, we can store file entries. These entries represent the root directory. Thus, the maximum number of entries we can have in the root directory is `256 - sizeof(header) / sizeof(entry) = 6`. This field is aligned on `sizeof(entry) = 32` bytes.

This diagram shows the organization of header, which is located in the first page, page 0:
![File System Header](img/header.jpg)

### Bitmap

The most important field of the header is the bitmap. Its role is to mark which 256-byte pages in the disk are free and which ones are allocated. Each bit represents a page, a `1` means allocated, a `0` means free.

As we have at most 256 pages on the disk, the bitmap size is `256/8 = 32` bytes. Bit `n` of byte `m` represents the page number `m * 8 + n`. Symmetrically, if we have a page number `p`, we can calculate its location in the bitmap, `m = p / 8` and `n = p % 8`.

For example, the page starting at disk offset `0x9A00` is page number `0x98`, it is represented by bit `0x9a % 8 = 2` of byte `0x9a / 8 = 19` in the bitmap.

In C, we can get its value with:

```C
const uint8_t byte = 0x9a / 8;
const uint8_t bit  = 0x9a % 8;
const uint8_t page_status = header->bitmap[byte] & (1 << bit);
```

The following **must** be true at all time:

* The number of 0 bits, marking a free page, in the whole bitmap is equal to the `free_pages` field from the header.
* The first bit of the bitmap (bit 0, byte 0) is 1, as page 0 is always allocated, it contains the file system header.

The following diagram is an example of a bitmap:
![File System Bitmap](img/bitmap.jpg)

In this example, the allocated pages are in red (bit is 1), and free pages are in green. (bit is 0)

### Directory and entry structure

All the directories in the file system have the same structure, they are composed of several entries: 6 for the root directory, 8 for other.

Each entry is 32-byte long and composed of:

* One byte of flags
* Name of the file/directory, 16-char long
* Page number where the content starts
* 16-bit size of the file, or `0x100` is case of a directory. The value is stored in little-endian.
* Date of creation. The date format is the same as in [Zeal 8-bit OS](https://github.com/Zeal8bit/Zeal-8-bit-OS/blob/main/include/time_h.asm), which is using BCD encoding.
* Reserved bytes, used for padding, and future extensions.

The following diagram shows the structure of the entries:
![File System Entry](img/entry.jpg)

Currently, the flag field is composed as followed:

* Bit 7: 1 if the entry is occupied, 0 if the entry is free
* *Bit 6..1: reserved/unused*
* Bit 0: 1 if the entry is a directory, 0 if the entry is a file

The name is 16-char long, it shall only contains ASCII printable characters, excluding `/` character. Lowercase and uppercase are both valid and are **not** equivalent. It can contain an extension, but in that case, the `.` is also part of the 16 characters. If the name of the entry is less than 16 characters, 0 bytes must be used as padding.

The page number marks the page containing the beginning of the content. If the entry is a directory, that start page itself contains again 8 entries. Thus, there is no limit in depth in the file system.
If the entry is a file, the start page contains the content as described in the next section.

### File content

The previous section describes how the directories are structured. The files' metadata are all contained in the entry structure, thus, they are part of the directory that contains the file.

The files are composed of one or several pages, which can be seen as a linked-list. Again, each page is 256 bytes, the first byte contains another page number, the remaining 255 bytes represent the file content.

That other page number points to the page that contains the remaining bytes of the file. If there are no other page, this byte **must** be 0. Of course, the page that is pointed by this number must also be a file page and not a directory page.

To calculate the amount of page a file requires, we need to calculate: `(file_size + 254) / 255`, in other words, we divide the file size by 255 and we round up the result. For example, a file containing 512 bytes will need 3 pages.
Let's imagine this file is named `alphabet.txt` contains the pattern "ABC...XYZABC...XYZ...", its structure would look like this:

![File System Page Content](img/filecontent.jpg)


## ZealFS v2

### Pages

In the version 2, the file system is still based around virtual pages. However, the sizes of the pages ranges from 256 bytes up to 64KB. Choosing the proper size is described below.

Pages are the smallest entities in the file system that can be allocated and freed. Thus, if the page size is 4KB, the smallest entities (files or directories) will have a size of at least 4KB.

### Header

The header has been slightly modified compared to the version 1, here are the fields that compose it:

* **Magic byte** — 1 byte, ASCII 'Z' (0x5A)
* **Version** — 1 byte, implementation version, must be 2 for ZealFS v2
* **Bitmap size** — 2 bytes, size of the bitmap in bytes
* **Free page count** — 2 bytes, number of available pages
* **Page size** — 1 byte, value representing the size of the pages in the file system, this value must be between 0 and 8 included:
    * 0 — 256-byte pages
    * 1 — 512-byte pages
    * 2 — 1KB pages
    * 3 — 2KB pages
    * 4 — 4KB pages
    * 5 — 8KB pages
    * 6 — 16KB pages
    * 7 — 32KB pages
    * 8 — 64KB pages
* **Page allocation bitmap** — variable size, decribed by the field `Bitmap size` above, see details below
* **Reserved area** — variable size, see details below
* **Root directory entries**

The first entries are similar to the version 1, except for the bitmap size which is now a 16-bit value, in little-endian.

The fifth field is a byte representing the size of the pages, size the maximum page size is 64KB, any value above `8` is invalid. It is possible to get the size of the pages in bytes thanks to the expression `1 << (8 + page_size)`, where `page_size` is taken from the header.

The sixth field is the bitmap, just like the version 1, it represents the pages that are allocated or freed. A bit of 1 marks an allocated page, while a bit 0 marks a free page. Since the number of pages and the page size are variable, the length of the bitmap will also be variable. Thanksfully, the field `bitmap size` is present and provides the size of the bitmap in bytes.

The following bytes are marked as _reserved_ and are mainly used for aligning the header on a `ZealFileEntry` bound (32 bytes). **The header must always be aligned on 32 bytes and must always fit in a single page**.

Therefore because of this, the page size must be choosen carefully to comply with this requirement. For example, for a 16MB disk, chooisng a 512-byte page size seems valid, but this would require `(16777216 / 512) / 8 = 32768` bytes for the bitmap, which exceeds the size of a single page. As such, for a

### Bitmap

Apart from its size, theory of the bitmap doesn't change from the version 1.

Getting the state (free/allocated) of a given page (32-bit) thanks to the bitmap can be done with:

```C
const uint32_t page_address = 0x12345678; // arbitrary byte address
const uint16_t page_index = page_address / page_size; // page_size in bytes!
const uint8_t byte = page_index / 8;
const uint8_t bit  = page_index % 8;
const uint8_t page_status = (header->bitmap[byte] >> bit) & 1;
```


### FAT Table

Unlike ZealFSv1, which did not use one, **ZealFSv2 introduces a FAT (File Allocation Table)** to track the linked list of pages that form files on the disk.

#### Layout

On **storages up to 64KB**, the FAT table contains up to 256 entries (1 byte per entry), allowing it to fit entirely within a single page. In this case:

* **Page 0**: Header and root directory
* **Page 1**: FAT table

On **storages larger than 64KB**, each FAT entry is 2 bytes (16 bits), supporting up to 65,536 entries. In this case:

* **Page 0**: Header and root directory
* **Pages 1–2**: FAT table (fixed size of exactly two pages)

> ⚠️ The FAT table **must always occupy exactly two pages**, regardless of storage size. This constraint simplifies the implementation and keeps metadata overhead low.


#### How the FAT Table Works

Each FAT entry at index `i`, denoted as `fat[i]`, stores the page number of the **next page in the file's chain**. This creates a simple singly-linked list of pages.
- If `fat[i]` is **non-zero**, it points to the next page.
- If `fat[i]` is **zero (0)**, then page `i` is the **last page** of the file/directory.

Index `i` represents the page that begins at byte offset `i * page_size` on the storage device. This means if `page_size = 512`, then fat[3] refers to the page starting at byte `3 * 512 = 1536` on the storage device.

#### Minimum Page Size by Storage Size

To ensure the FAT table fits within the two-page limit, the minimum page size required depends on the total storage size:

| Storage Size Range          | Minimum Page Size |
|----------------------------|-------------------|
| 0 < `disk_size` ≤ 64KB      | 256 bytes         |
| 64KB < `disk_size` ≤ 256KB  | 512 bytes         |
| 256KB < `disk_size` ≤ 1MB   | 1KB               |
| 1MB < `disk_size` ≤ 4MB     | 2KB               |
| 4MB < `disk_size` ≤ 16MB    | 4KB               |
| 16MB < `disk_size` ≤ 64MB   | 8KB               |
| 64MB < `disk_size` ≤ 256MB  | 16KB              |
| 256MB < `disk_size` ≤ 1GB   | 32KB              |
| 1GB < `disk_size` ≤ 4GB     | 64KB              |


This design allows ZealFSv2 to scale smoothly from small-memory devices to multi-gigabyte storage systems with minimal overhead.


### Directory and entry structure

All the directories in the file system have the same structure, they are composed of several entries.

Each entry is 32-byte long and composed of:

* One byte of flags
* Name of the file/directory, 16-char long
* 16-bit page number where the content starts
* 32-bit size of the file (`page_size` in bytes for directories). The value is stored in little-endian.
* Date of creation. The date format is the same as in [Zeal 8-bit OS](https://github.com/Zeal8bit/Zeal-8-bit-OS/blob/main/include/time_h.asm), which is using BCD encoding.
* Reserved bytes, used for padding, and future extensions.

Currently, the flag field is composed as followed:

* Bit 7: 1 if the entry is occupied, 0 if the entry is free
* *Bit 6..1: reserved/unused*
* Bit 0: 1 if the entry is a directory, 0 if the entry is a file

The name is 16-char long, it shall only contains ASCII printable characters, excluding `/` character. Lowercase and uppercase are both valid and are **not** equivalent. It can contain an extension, but in that case, the `.` is also part of the 16 characters. If the name of the entry is less than 16 characters, 0 bytes must be used as padding.

The page number marks the page containing the beginning of the content. If the entry is a directory, that start page itself contains entries again. Thus, there is no limit in depth in the file system.
If the entry is a file, the start page contains the content as described in the next section.

In ZealFS v2, directories are not limited in the number of entries they can hold. When a directory page becomes full, an additional page is allocated and linked using the FAT table. Therefore, directory entries may span across multiple pages.

> ⚠️ Important: When a file is deleted, its directory entry is simply marked as free, but the rest of the linked pages may still contain valid entries. This means that even if a page appears to have empty slots, you must continue following the chain of pages to ensure all entries are properly read.

### File Content

In ZealFS v2, all file metadata—such as name, size, and timestamps—are stored within the directory entry itself. This means files do not carry additional metadata within their data pages.

Files are stored across one or more pages. Thanks to the FAT table, **the pages allocated for file content contain only raw data**. Unlike ZealFS v1, the first byte of each page no longer needs to hold a reference to the next page. Page chaining is now entirely managed by the FAT table, keeping data pages free of metadata and maximizing usable space.

To determine how many pages a file requires, use the following formula:
```
required_pages = (file_size + page_size - 1) / page_size
```

This calculates the number of pages needed to store the file by dividing the file size by the page size and rounding up.

For example, if `file_size = 1300` bytes and `page_size = 512`, then:
```
required_pages = (1300 + 512 - 1) / 512 = 1811 / 512 ≈ 3.53 → 4 pages
```

So the file would occupy 4 pages.

### File content

The previous section describes how the directories are structured. The files metadata are all contained in the entry structure, thus, they are part of the directory that contains the file.

The files are also composed of one or several page. Thanks to the presenc eof the FAT table, the first byte of files' pages are **not** other page indexes anymore, they directly contain the file data. This means that the pages allocated for files only contain data, no metadata. The FAT table is in charge of storing the link of pages.


To calculate the amount of page a file requires, we need to calculate: `(file_size +  page_size - 1)/ page_size`, where `page_size` is the size of the pages in bytes. In other words, we divide the file size by the size of a page and we round up the result.

## License

Distributed under the Apache 2.0 License. See LICENSE file for more information.

You are free to use it for personal and commercial use, the boilerplate present in each file must not be removed.

## Contact

For any suggestion or request, you can contact me at contact [at] zeal8bit [dot] com

For bug fixes, or questions, you can also open a pull request or an issue.
