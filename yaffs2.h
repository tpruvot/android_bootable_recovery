#ifndef RECOVERY_YAFFS2_H
#define RECOVERY_YAFFS2_H

typedef void (*mkyaffs2image_callback) (char* filename);

int mkyaffs2image(char* target_directory, char* filename, int fixstats, mkyaffs2image_callback callback);

typedef void (*unyaffs_callback) (char* filename);

int unyaffs(char* filename, char* directory, unyaffs_callback callback);

#endif
