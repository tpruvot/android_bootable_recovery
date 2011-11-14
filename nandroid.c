#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>
#include <sys/wait.h>
#include <sys/limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statfs.h>

#include <signal.h>
#include <sys/wait.h>

#include "bootloader.h"
#include "common.h"
#include "cutils/properties.h"
#include "firmware.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "recovery_ui.h"
#include "libcrecovery/common.h"

#include <sys/vfs.h>

#include "extendedcommands.h"
#include "nandroid.h"
#include "mounts.h"

#include "flashutils/flashutils.h"
#include <libgen.h>

#include EXPAND(BUILD_TOP/external/yaffs2/yaffs2/utils/mkyaffs2image.h)
#include EXPAND(BUILD_TOP/external/yaffs2/yaffs2/utils/unyaffs.h)

void nandroid_generate_timestamp_path(const char* backup_path)
{
    time_t t = time(NULL);
    struct tm *tmp = localtime(&t);
    if (tmp == NULL)
    {
        struct timeval tp;
        gettimeofday(&tp, NULL);
        sprintf((char*) backup_path, "/sdcard/clockworkmod/backup/%d", (int) tp.tv_sec);
    }
    else
    {
        strftime((char*) backup_path, PATH_MAX, "/sdcard/clockworkmod/backup/%F.%H.%M.%S", tmp);
    }
}

static int print_and_error(const char* message) {
    ui_print("%s", message);
    return 1;
}

static int yaffs_files_total = 0;
static int yaffs_files_count = 0;
static void yaffs_callback(const char* filename)
{
    if (filename == NULL)
        return;
    const char* justfile = basename(filename);
    char tmp[PATH_MAX];
    strcpy(tmp, justfile);
    if (tmp[strlen(tmp) - 1] == '\n')
        tmp[strlen(tmp) - 1] = '\0';
    if (strlen(tmp) < 30)
        ui_print("%s", tmp);
    yaffs_files_count++;
    if (yaffs_files_total != 0)
        ui_set_progress((float)yaffs_files_count / (float)yaffs_files_total);
    ui_reset_text_col();
}

static void compute_directory_stats(const char* directory)
{
    char tmp[PATH_MAX];
    sprintf(tmp, "find '%s' | wc -l > /tmp/dircount", directory);
    __system(tmp);
    char count_text[100];
    FILE* f = fopen("/tmp/dircount", "r");
    fread(count_text, 1, sizeof(count_text), f);
    fclose(f);
    yaffs_files_count = 0;
    yaffs_files_total = atoi(count_text);
    ui_reset_progress();
    ui_show_progress(1, 0);
}

typedef void (*file_event_callback)(const char* filename);
typedef int (*nandroid_backup_handler)(const char* backup_path, const char* backup_file_image, int callback);

static int mkyaffs2image_wrapper(const char* backup_path, const char* backup_file_image, int callback) {
    char backup_file_image_with_extension[PATH_MAX];
    sprintf(backup_file_image_with_extension, "%s.img", backup_file_image);
    return mkyaffs2image((char*) backup_path, backup_file_image_with_extension, 0, callback ? (mkyaffs2image_callback) yaffs_callback : NULL);
}

static int tar_compress_wrapper(const char* backup_path, const char* backup_file_image, int callback) {
    char tmp[PATH_MAX];
    if (strcmp(backup_path, "/data") == 0 && volume_for_path("/sdcard") == NULL)
      sprintf(tmp, "cd $(dirname %s) ; tar cvf '%s.tar' --exclude 'media' $(basename %s) ; exit $?", backup_path, backup_file_image, backup_path);
    else
      sprintf(tmp, "cd $(dirname %s) ; tar cvf '%s.tar' $(basename %s) ; exit $?", backup_path, backup_file_image, backup_path);

    FILE *fp = __popen(tmp, "r");
    if (fp == NULL) {
        ui_print("Unable to execute tar.\n");
        return -1;
    }

    while (fgets(tmp, PATH_MAX, fp) != NULL) {
        tmp[PATH_MAX - 1] = '\0';
        if (callback)
            yaffs_callback(tmp);
    }

    return __pclose(fp);
}

static nandroid_backup_handler get_backup_handler(const char *backup_path) {
    Volume *v = volume_for_path(backup_path);
    if (v == NULL) {
        ui_print("Unable to find volume.\n");
        return NULL;
    }
    MountedVolume *mv = (MountedVolume*) find_mounted_volume_by_mount_point(v->mount_point);
    if (mv == NULL) {
        ui_print("Unable to find mounted volume: %s\n", v->mount_point);
        return NULL;
    }

    if (strcmp(backup_path, "/data") == 0 && is_data_media()) {
        return tar_compress_wrapper;
    }

    // cwr5, we prefer tar for everything except yaffs2
    if (strcmp("yaffs2", mv->filesystem) == 0) {
        return mkyaffs2image_wrapper;
    }

    char str[255];
    char* partition;
    property_get("ro.cwm.prefer_tar", str, "true");
    if (strcmp("true", str) != 0) {
        return mkyaffs2image_wrapper;
    }

    return tar_compress_wrapper;
}


int nandroid_backup_partition_extended(const char* backup_path, const char* mount_point, int umount_when_finished) {
    int ret = 0;

    ui_print("%s:\n", basename(backup_path));

    char* name = basename(mount_point);

    struct stat file_info;
    int callback = stat("/sdcard/clockworkmod/.hidenandroidprogress", &file_info) != 0;
    
    ui_print("Backing up %s...\n", name);
    if (0 != (ret = ensure_path_mounted(mount_point) != 0)) {
        LOGW("Can't mount %s!\n", mount_point);
        return ret;
    }
    compute_directory_stats(mount_point);
    char tmp[PATH_MAX];
    char tmp_name[PATH_MAX];
    strcpy(tmp_name,name);
    scan_mounted_volumes();
    Volume *v = volume_for_path(mount_point);
    MountedVolume *mv = NULL;
    if (v != NULL)
        mv = (MountedVolume*) find_mounted_volume_by_mount_point(v->mount_point);
    if (mv == NULL || mv->filesystem == NULL)
        sprintf(tmp, "%s/%s.auto", backup_path, name);
    else
        sprintf(tmp, "%s/%s.%s", backup_path, name, mv->filesystem);
    nandroid_backup_handler backup_handler = get_backup_handler(mount_point);
    if (backup_handler == NULL) {
        ui_print("Error finding an appropriate backup handler.\n");
        return -2;
    }
    ret = backup_handler(mount_point, tmp, callback);
    if (umount_when_finished) {
        ensure_path_unmounted(mount_point);
    }
    if (0 != ret) {
        ui_print("Error while making a backup image of %s!\n", mount_point);
        return ret;
    }
    ui_print("Generating %s md5 sum...\n", tmp_name);
    sprintf(tmp, "nandroid-md5.sh '%s' %s", backup_path, tmp_name);
    if (__system(tmp))
        ui_print("Error while generating %s md5 sum!\n", tmp_name);
    return 0;
}

int nandroid_backup_partition(const char* backup_path, const char* root) {
    Volume *vol = volume_for_path(root);
    // make sure the volume exists before attempting anything...
    if (vol == NULL || vol->fs_type == NULL)
        return 0;

    // see if we need a raw backup (mtd)
    char tmp[PATH_MAX];
    int ret;
    if (strcmp(vol->fs_type, "mtd") == 0 ||
            strcmp(vol->fs_type, "bml") == 0 ||
            strcmp(vol->fs_type, "emmc") == 0) {
        const char* name = basename(root);
        sprintf(tmp, "%s/%s.img", backup_path, name);
        ui_print("Backing up %s image...\n", name);
        if (0 != (ret = backup_raw_partition(vol->fs_type, vol->device, tmp))) {
            ui_print("Error while backing up %s image!", name);
            return ret;
        }
        ui_print("Generating %s md5 sum...\n", name);
        sprintf(tmp, "nandroid-md5.sh '%s' %s", backup_path, name);
        if (__system(tmp))
            ui_print("Error while generating %s md5 sum!\n", name);
        return 0;
    }

    return nandroid_backup_partition_extended(backup_path, root, 0);
}

int nandroid_backup(const char* backup_path, int parts)
{
    ui_set_background(BACKGROUND_ICON_INSTALLING);
    
    if (ensure_path_mounted(backup_path) != 0) {
        return print_and_error("Can't mount backup path.\n");
    }
    
    Volume* volume = volume_for_path(backup_path);
    if (NULL == volume)
        return print_and_error("Unable to find volume for backup path.\n");
    int ret=0;
    struct stat st;
    struct statfs s;
    if (0 != (ret = statfs(volume->mount_point, &s)))
        return print_and_error("Unable to stat backup path.\n");
    uint64_t bavail = s.f_bavail;
    uint64_t bsize = s.f_bsize;
    uint64_t sdcard_free = bavail * bsize;
    uint64_t sdcard_free_mb = sdcard_free / (uint64_t)(1024 * 1024);
    ui_print("SD Card space free: %lluMB\n", sdcard_free_mb);
    if (sdcard_free_mb < 150)
        ui_print("There may not be enough free space to complete backup... continuing...\n");

    const char * warning = "\nProblem while backing up %s !\n";
    char tmp[PATH_MAX];
    sprintf(tmp, "mkdir -p %s", backup_path);
    __system(tmp);

    if ((parts & BAK_BOOT) && (ret = nandroid_backup_partition(backup_path, "/boot")) )
        ui_print(warning, "boot");

    if ((parts & BAK_SYSTEM) && (ret = nandroid_backup_partition_extended(backup_path, "/system", 0)) )
        ui_print(warning, "system");

    if ((parts & BAK_DATA) && (ret = nandroid_backup_partition_extended(backup_path, "/data", 0)) )
        ui_print(warning, "data");

    if (has_datadata()) {
        if ((parts & BAK_DATA) && (ret = nandroid_backup_partition(backup_path, "/datadata")))
            ui_print(warning, "datadata");
    }

    if ((parts & BAK_DATA) && stat("/sdcard/.android_secure", &st))
        ui_print("No /sdcard/.android_secure found. Skipping backup of applications on external storage.\n");
    else if ((parts & BAK_DATA) && (ret = nandroid_backup_partition_extended(backup_path, "/sdcard/.android_secure", 0)))
        ui_print(warning, "android_secure");

    if ((parts & BAK_CACHE) && (ret = nandroid_backup_partition_extended(backup_path,"/cache", 0)))
        ui_print(warning, "cache");

    if ((parts & BAK_DEVTREE) && (ret = nandroid_backup_partition(backup_path,"/devtree")))
        ui_print(warning, "devtree");

    if ((parts & BAK_LOGO) && (ret = nandroid_backup_partition(backup_path,"/logo")))
        ui_print(warning, "logo");

    if ((parts & BAK_RECOVERY) && (ret = nandroid_backup_partition(backup_path,"/recovery")))
        ui_print(warning, "recovery");

/*
    Volume *vol = volume_for_path("/wimax");
    if (parts & BAK_WIMAX)
    {
        if (vol != NULL && 0 == statfs(vol->device, &s))
        {
            char serialno[PROPERTY_VALUE_MAX];
            ui_print("Backing up WiMAX...\n");
            serialno[0] = 0;
            property_get("ro.serialno", serialno, "");
            sprintf(tmp, "%s/wimax.%s.img", backup_path, serialno);
            ret = backup_raw_partition(vol->fs_type, vol->device, tmp);
            ui_print("Generating pds.%s md5 sum...\n", serialno);
            sprintf(tmp, "nandroid-md5.sh '%s' 'wimax.%s'", backup_path, serialno);
            if (__system(tmp))
                ui_print("Error while generating pds.%s md5 sum!\n", serialno);
            if (0 != ret)
                ui_print(warning, "WiMAX");
                //return print_and_error("Error while dumping WiMAX image!\n");
        }
    }
*/
    if ((parts & BAK_PDS) && (ret = nandroid_backup_partition_extended(backup_path,"/pds", 0)))
        ui_print(warning, "pds");

    Volume *vol = volume_for_path("/sd-ext");
    if ((parts & BAK_SDEXT) && (vol == NULL || 0 != statfs(vol->device, &s)))
    {
        ui_print("No sd-ext found. Skipping backup of sd-ext.\n");
    }
    else
    {
        if ((parts & BAK_SDEXT) && ensure_path_mounted("/sd-ext"))
            ui_print("Could not mount sd-ext. sd-ext backup may not be supported on this device. Skipping backup of sd-ext.\n");
        else if ((parts & BAK_SDEXT) && (ret = nandroid_backup_partition(backup_path,"/sd-ext")))
            ui_print(warning, "sd-ext");
    }

    sync();
    ui_set_background(BACKGROUND_ICON_NONE);
    ui_reset_progress();
    if (ret == 0)
        ui_print("\nBackup complete!\n");

    return ret;
}

typedef int (*format_function)(char* root);

static void ensure_directory(const char* dir) {
    char tmp[PATH_MAX];
    sprintf(tmp, "mkdir -p %s", dir);
    __system(tmp);
}

typedef int (*nandroid_restore_handler)(const char* backup_file_image, const char* backup_path, int callback);

static int unyaffs_wrapper(const char* backup_file_image, const char* backup_path, int callback) {
    return unyaffs((char*) backup_file_image, (char*) backup_path, callback ? (unyaffs_callback) yaffs_callback : NULL);
}

static int tar_extract_wrapper(const char* backup_file_image, const char* backup_path, int callback) {
    char tmp[PATH_MAX];
    sprintf(tmp, "cd $(dirname %s) ; tar xvf '%s' ; exit $?", backup_path, backup_file_image);

    char path[PATH_MAX];
    FILE *fp = __popen(tmp, "r");
    if (fp == NULL) {
        ui_print("Unable to execute tar.\n");
        return -1;
    }

    while (fgets(path, PATH_MAX, fp) != NULL) {
        if (callback)
            yaffs_callback(path);
    }

    return __pclose(fp);
}

static nandroid_restore_handler get_restore_handler(const char *backup_path) {
    Volume *v = volume_for_path(backup_path);
    if (v == NULL) {
        ui_print("Unable to find volume.\n");
        return NULL;
    }
    scan_mounted_volumes();
    MountedVolume *mv = (MountedVolume*) find_mounted_volume_by_mount_point(v->mount_point);
    if (mv == NULL) {
        ui_print("Unable to find mounted volume: %s\n", v->mount_point);
        return NULL;
    }

    if (strcmp(backup_path, "/data") == 0 && is_data_media()) {
        return tar_extract_wrapper;
    }

    // cwr 5, we prefer tar for everything unless it is yaffs2
    char str[255];
    char* partition;
    property_get("ro.cwm.prefer_tar", str, "false");
    if (strcmp("true", str) != 0) {
        return unyaffs_wrapper;
    }

    if (strcmp("yaffs2", mv->filesystem) == 0) {
        return unyaffs_wrapper;
    }

    return tar_extract_wrapper;
}

int nandroid_restore_partition_extended(const char* backup_path, const char* mount_point, int umount_when_finished) {
    int ret = 0;
    char* name = basename(mount_point);

    nandroid_restore_handler restore_handler = NULL;
    const char *filesystems[] = { "yaffs2", "ext2", "ext3", "ext4", "vfat", "rfs", NULL };
    const char* backup_filesystem = NULL;
    Volume *vol = volume_for_path(mount_point);
    const char *device = NULL;
    if (vol != NULL)
        device = vol->device;

    char tmp[PATH_MAX];
    sprintf(tmp, "%s/%s.img", backup_path, name);
    struct stat file_info;
    if (0 != (ret = stat(tmp, &file_info))) {
        // can't find the backup, it may be the new backup format?
        // iterate through the backup types
        LOGI("couldn't find default type\n");
        const char *filesystem;
        int i = 0;
        while ((filesystem = filesystems[i]) != NULL) {
            sprintf(tmp, "%s/%s.%s.img", backup_path, name, filesystem);
            if (0 == (ret = stat(tmp, &file_info))) {
                backup_filesystem = filesystem;
                restore_handler = unyaffs_wrapper;
                break;
            }
            sprintf(tmp, "%s/%s.%s.tar", backup_path, name, filesystem);
            if (0 == (ret = stat(tmp, &file_info))) {
                backup_filesystem = filesystem;
                restore_handler = tar_extract_wrapper;
                break;
            }
            i++;
        }

        if (backup_filesystem == NULL || restore_handler == NULL) {
            ui_print("%s.img not found.\nSkipping restore of %s.\n", name, mount_point);
            return 0;
        }
        else {
            printf("Found new backup image: %s\n", tmp);
        }

        // If the fs_type of this volume is "auto", let's revert to using a
        // rm -rf, rather than trying to do a ext3/ext4/whatever format.
        // This is because some phones (like DroidX) will freak out if you
        // reformat the /system or /data partitions, and not boot due to
        // a locked bootloader.
        // The "auto" fs type preserves the file system, and does not
        // trigger that lock.
        // Or of volume does not exist (.android_secure), just rm -rf.
        if (vol == NULL || 0 == strcmp(vol->fs_type, "auto"))
            backup_filesystem = NULL;
    }

    ensure_directory(mount_point);

    int callback = stat("/sdcard/clockworkmod/.hidenandroidprogress", &file_info) != 0;

    ui_print("Restoring %s...\n", name);

    if (strcmp(mount_point,"/sd-ext") == 0) {
        ui_print("Formatting %s, please wait...\n", name);
    }
    if (backup_filesystem == NULL) {
        if (0 != (ret = format_volume(mount_point))) {
            ui_print("Error while formatting volume %s!\n", mount_point);
            return ret;
        }
    }
    else if (0 != (ret = format_device(device, mount_point, backup_filesystem))) {
        ui_print("Error while formatting device %s!\n", mount_point);
        ui_printlogtail(4);
        return ret;
    }

    if (0 != (ret = ensure_path_mounted(mount_point)) ) {
        if (strcmp(vol->fs_type,"emmc") != 0)
            ui_print("Can't mount %s!\n", mount_point);
        return ret;
    }

    if (restore_handler == NULL)
        restore_handler = get_restore_handler(mount_point);
    if (restore_handler == NULL) {
        ui_print("Error finding an appropriate restore handler.\n");
        return -2;
    }
    if (0 != (ret = restore_handler(tmp, mount_point, callback))) {
        ui_print("Error while restoring %s!\n", mount_point);
        return ret;
    }

    if (umount_when_finished) {
        ensure_path_unmounted(mount_point);
    }

    if (ret == 0 && strcmp(mount_point,"/pds") == 0) {
        ensure_path_mounted("/data");
        __system("cp /tmp/pds.img /data/pds.img");
    }
    return 0;
}

int nandroid_restore_partition(const char* backup_path, const char* root) {
    Volume *vol = volume_for_path(root);
    // make sure the volume exists...
    if (vol == NULL || vol->fs_type == NULL)
        return 0;

    // see if we need a raw restore (mtd)
    char tmp[PATH_MAX];
    if (strcmp(vol->fs_type, "mtd") == 0 ||
            strcmp(vol->fs_type, "bml") == 0 ||
            strcmp(vol->fs_type, "emmc") == 0) {
        int ret, erase=1;
        const char* name = basename(root);
        if (!strcmp(name, "devtree") || !strcmp(name, "logo")) {
            erase=0;
        }
        if (erase) {
            ui_print("Erasing %s before restore...\n", name);
            if (0 != (ret = format_volume(root))) {
                ui_print("Error while erasing %s image!", name);
                return ret;
            }
        }
        sprintf(tmp, "%s%s.img", backup_path, root);
        ui_print("Restoring %s image...\n", name);
        if (0 != (ret = restore_raw_partition(vol->fs_type, vol->device, tmp))) {
            ui_print("Error while flashing %s image!", name);
            return ret;
        }
        return 0;
    }
    return nandroid_restore_partition_extended(backup_path, root, 1);
}

int nandroid_restore(const char* backup_path, int parts)
{
    ui_set_background(BACKGROUND_ICON_INSTALLING);
    ui_show_indeterminate_progress();
    yaffs_files_total = 0;

    if (ensure_path_mounted(backup_path) != 0)
        return print_and_error("Can't mount backup path\n");

    char tmp[PATH_MAX];

    ui_print("Checking MD5 sums...\n");
    sprintf(tmp, "cd '%s' && cat *.md5 > md5", backup_path);
    __system(tmp);

    char* fmt = "cd '%s' && cat md5 | grep -v %s > md5_filtered && cp md5_filtered md5";
    if (!(parts & BAK_BOOT)) {
       sprintf(tmp, fmt, backup_path, "boot");
       __system(tmp);
    }
    if (!(parts & BAK_SYSTEM)) {
       sprintf(tmp, fmt, backup_path, "system");
       __system(tmp);
    }
    if (!(parts & BAK_DATA)) {
       sprintf(tmp, fmt, backup_path, "data");
       __system(tmp);
       sprintf(tmp, fmt, backup_path, "android_secure");
       __system(tmp);
    }
    if (!(parts & BAK_CACHE)) {
       sprintf(tmp, fmt, backup_path, "cache");
       __system(tmp);
    }
    if (!(parts & BAK_SDEXT)) {
       sprintf(tmp, fmt, backup_path, "sd-ext");
       __system(tmp);
    }
    if (!(parts & BAK_DEVTREE)) {
       sprintf(tmp, fmt, backup_path, "devtree");
       __system(tmp);
    }
    if (!(parts & BAK_RECOVERY)) {
       sprintf(tmp, fmt, backup_path, "recovery");
       __system(tmp);
    }
    if (!(parts & BAK_LOGO)) {
       sprintf(tmp, fmt, backup_path, "logo");
       __system(tmp);
    }
    if (!(parts & BAK_PDS)) {
       sprintf(tmp, fmt, backup_path, "pds");
       __system(tmp);
    }
    sprintf(tmp, "cd '%s' && md5sum -c md5", backup_path);
    int ret = __system(tmp);
    sprintf(tmp, "cd '%s' && rm -f md5 md5_filtered", backup_path);
    __system(tmp);
    if(ret)
        return print_and_error("MD5 mismatch!\n");

    //BACKUP START HERE

    if ((parts & BAK_BOOT) && NULL != volume_for_path("/boot") && 0 != (ret = nandroid_restore_partition(backup_path, "/boot")))
        ui_print("\nProblem while restoring boot !\n");

/*
    struct stat s;
    Volume *vol = volume_for_path("/wimax");
    if ((parts & BAK_WIMAX) && vol != NULL && 0 == stat(vol->device, &s))
    {
        char serialno[PROPERTY_VALUE_MAX];
        serialno[0] = 0;
        property_get("ro.serialno", serialno, "");
        sprintf(tmp, "%s/wimax.%s.img", backup_path, serialno);

        struct stat st;
        if (0 != stat(tmp, &st))
        {
            ui_print("WARNING: WiMAX partition exists, but nandroid\n");
            ui_print("         backup does not contain WiMAX image.\n");
            ui_print("         You should create a new backup to\n");
            ui_print("         protect your WiMAX keys.\n");
        }
        else
        {
            ui_print("Erasing WiMAX before restore...\n");
            if (0 != (ret = format_volume("/wimax")))
                return print_and_error("Error while formatting wimax!\n");
            ui_print("Restoring PDS image...\n");
            if (0 != (ret = restore_raw_partition(vol->fs_type, vol->device, tmp)))
                return ret;
        }
    }
*/

    if ((parts & BAK_SYSTEM) && 0 != (ret = nandroid_restore_partition(backup_path, "/system")))
        ui_print("\nProblem while restoring system !\n");

    if ((parts & BAK_DATA) && 0 != (ret = nandroid_restore_partition(backup_path, "/data")))
        ui_print("\nProblem while restoring data !\n");

    if (has_datadata()) {
        if ((parts & BAK_DATA) && 0 != (ret = nandroid_restore_partition(backup_path, "/datadata")))
            ui_print("\nProblem while restoring datadata !\n");
    }

    if ((parts & BAK_DATA) && 0 != (ret = nandroid_restore_partition_extended(backup_path, "/sdcard/.android_secure", 0)))
        ui_print("\nProblem while restoring android_secure !\n");

    if ((parts & BAK_CACHE) && 0 != (ret = nandroid_restore_partition_extended(backup_path, "/cache", 0)))
        ui_print("\nProblem while restoring cache !\n");

    if ((parts & BAK_SDEXT) && 0 != (ret = nandroid_restore_partition_extended(backup_path, "/sd-ext", 1)))
        ui_print("\nProblem while restoring sd-ext !\n");

    if ((parts & BAK_DEVTREE) && 0 != (ret = nandroid_restore_partition(backup_path, "/devtree")))
        ui_print("\nProblem while restoring devtree !\n");

    if ((parts & BAK_RECOVERY) && 0 != (ret = nandroid_restore_partition(backup_path, "/recovery")))
        ui_print("\nProblem while restoring recovery !\n");

    if ((parts & BAK_LOGO) && 0 != (ret = nandroid_restore_partition(backup_path, "/logo")))
        ui_print("\nProblem while restoring logo !\n");

    if ((parts & BAK_PDS) && 0 != (ret = nandroid_restore_partition_extended(backup_path, "/pds", 1)))
        ui_print("\nProblem while restoring pds !\n");

    sync();
    ui_set_background(BACKGROUND_ICON_NONE);
    ui_reset_progress();
    if (ret == 0)
        ui_print("\nRestore complete!\n");

    return 0;
}

int nandroid_usage()
{
    printf("Usage: nandroid backup\n");
    printf(" nandroid backup\n");
    printf(" nandroid restore <directory>\n");
    printf(" nandroid restore-boot <directory>\n");
    return 1;
}

int nandroid_main(int argc, char** argv)
{
    if (argc > 3 || argc < 2)
        return nandroid_usage();

    if (strcmp("backup", argv[1]) == 0)
    {
        if (argc != 2)
            return nandroid_usage();
        
        char backup_path[PATH_MAX];
        nandroid_generate_timestamp_path(backup_path);
        return nandroid_backup(backup_path, BACKUP_ALL);
    }

    if (strcmp("restore", argv[1]) == 0)
    {
        if (argc != 3)
            return nandroid_usage();

        return nandroid_restore(argv[2], BAK_SYSTEM | BAK_DATA);
    }

    if (strcmp("restore-boot", argv[1]) == 0)
    {
        if (argc != 3)
            return nandroid_usage();

        return nandroid_restore(argv[2], BAK_BOOT | BAK_DEVTREE | BAK_RECOVERY | BAK_LOGO);
    }
    return nandroid_usage();
}
