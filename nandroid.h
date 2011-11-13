#ifndef NANDROID_H
#define NANDROID_H

#define BAK_RECOVERY 0x01
#define BAK_BOOT     0x02
#define BAK_SYSTEM   0x04
#define BAK_DATA     0x08
#define BAK_CACHE    0x10
#define BAK_SDEXT    0x20

#define BAK_PDS     0x100
#define BAK_DEVTREE 0x200
#define BAK_WIMAX   0x400
#define BAK_LOGO    0x800

#define BACKUP_ALL   BAK_SYSTEM | BAK_DATA | BAK_BOOT | BAK_DEVTREE | BAK_RECOVERY | BAK_LOGO

int nandroid_main(int argc, char** argv);
int nandroid_backup(const char* backup_path, int parts);
int nandroid_restore(const char* backup_path, int parts);

#endif //NANDROID_H
