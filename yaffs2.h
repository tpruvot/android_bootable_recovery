#ifndef RECOVERY_YAFFS2_H
#define RECOVERY_YAFFS2_H

#ifdef USE_EXT4
#include "make_ext4fs.h"
#define make_ext4fs(filename, dir, mp, c, d, e) \
        make_ext4fs_wrapper(filename, dir, mp, c, d, e)
int make_ext4fs_wrapper(const char *filename, const char *directory,
                char *mountpoint, fs_config_func_t fs_config_func, int gzip, int sparse);
#endif

typedef void (*mkyaffs2image_callback) (char* filename);

int mkyaffs2image(char* target_directory, char* filename, int fixstats, mkyaffs2image_callback callback);

typedef void (*unyaffs_callback) (char* filename);

int unyaffs(char* filename, char* directory, unyaffs_callback callback);

#endif
