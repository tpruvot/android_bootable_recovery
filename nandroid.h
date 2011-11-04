#ifndef NANDROID_H
#define NANDROID_H

#define BAK_RECOVERY 0x01
#define BAK_BOOT     0x02
#define BAK_SYSTEM   0x04
#define BAK_DATA     0x08
#define BAK_CACHE    0x10
#define BAK_SDEXT    0x20
#define BAK_PDS      0x40
#define BAK_DEVTREE  0x80

#define BACKUP_ALL   BAK_SYSTEM | BAK_DATA | BAK_BOOT | BAK_DEVTREE | BAK_RECOVERY | BAK_PDS

int nandroid_main(int argc, char** argv);
int nandroid_backup(const char* backup_path, int parts);
int nandroid_restore(const char* backup_path, int parts);

//int nandroid_backup(const char* backup_path, int backup_recovery, int backup_boot, int backup_system, int backup_data, int backup_cache, int backup_sdext, int backup_pds);
//int nandroid_restore(const char* backup_path, int restore_boot, int restore_system, int restore_data, int restore_cache, int restore_sdext, int restore_pds);

#endif
