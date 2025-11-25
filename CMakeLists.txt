cmake_minimum_required(VERSION 3.10)

project(zealfs C)

set(SRCS
    src/zealfs_main.c
    src/zealfs_v1.c
    src/zealfs_v2.c
    src/mbr.c
)
add_executable(zealfs ${SRCS})
target_include_directories(zealfs PRIVATE "include")


# Find FUSE3 add cflags and libs from pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE3 REQUIRED fuse3)

target_include_directories(zealfs PRIVATE ${FUSE3_INCLUDE_DIRS})
target_compile_options(zealfs PRIVATE ${FUSE3_CFLAGS_OTHER})
target_link_libraries(zealfs PRIVATE ${FUSE3_LIBRARIES})
target_link_options(zealfs PRIVATE ${FUSE3_LDFLAGS_OTHER})
