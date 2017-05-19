#ifndef _RKIMAGE_H_
#define _RKIMAGE_H_

#include "minzip/Zip.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PART_NAME				32

#define RELATIVE_PATH				64

#define MAX_PARTS				20

#define MAX_MACHINE_MODEL		64

//#define MAX_MANUFACTURER		64
#define MAX_MANUFACTURER		60

#define MAX_PACKAGE_FILES			16

typedef struct tagRKIMAGE_ITEM
{
	char name[PART_NAME];// 锟斤拷锟斤拷锟斤拷锟�
	char file[RELATIVE_PATH];// 锟斤拷锟铰凤拷锟斤拷锟斤拷锟饺★拷募锟绞憋拷玫锟�
	unsigned int offset;// 锟侥硷拷锟斤拷Image锟叫碉拷偏锟斤拷
	unsigned int flash_offset;// 锟斤拷写锟斤拷Flash锟叫碉拷位锟斤拷(锟斤拷sector为锟斤拷位)
	unsigned int usespace;// 锟侥硷拷占锟矫空间（锟斤拷PAGE锟斤拷锟斤拷)
	unsigned int size;// 锟街斤拷锟斤拷实锟斤拷锟侥硷拷锟斤拷小
}RKIMAGE_ITEM;

typedef struct tagRKIMAGE_HDR
{
	unsigned int tag;
	unsigned int size;// 锟侥硷拷锟斤拷小锟斤拷锟斤拷锟斤拷末尾锟斤拷CRC校锟斤拷锟斤拷
	char machine_model[MAX_MACHINE_MODEL];
	char manufacturer[MAX_MANUFACTURER];
	unsigned int version;
	int item_count;
	RKIMAGE_ITEM item[MAX_PACKAGE_FILES];
}RKIMAGE_HDR;

//copy from updater.h
typedef struct {
    FILE* cmd_pipe;
    ZipArchive* package_zip;
    int version;
} UpdaterInfo;

#define RKIMAGE_TAG				0x46414B52

//#define PAGESIZE				2048

#define BOOTLOADER				"Rock28Boot(L).bin"
#define PARTNAME_BOOTLOADER		"bootloader"

#define RK_UPDATE_SCRIPT			"update-script"
#define RK_RECOVER_SCRIPT			"recover-script"


int install_rkimage(const char* update_file);
int recover_backup(const char *root_path);

#ifdef __cplusplus
}
#endif

#endif
