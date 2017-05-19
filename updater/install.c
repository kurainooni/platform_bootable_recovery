/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#include "cutils/misc.h"
#include "cutils/properties.h"
#include "edify/expr.h"
#include "mincrypt/sha.h"
#include "minzip/DirUtil.h"
#include "mtdutils/mounts.h"
#include "mtdutils/mtdutils.h"
#include "updater.h"
#include "applypatch/applypatch.h"
#include "cutils/android_reboot.h"

#ifdef USE_EXT4
#include "make_ext4fs.h"
#endif

#include "mtdutils/rk29.h"

static int set_bootloader_message(const struct bootloader_message *in);
static int set_bootloader_message_block_rk29(const struct bootloader_message *in, const Volume* v);
static int set_bootloader_message_mtd(const struct bootloader_message *in, const Volume* v);
static int set_bootloader_message_block(const struct bootloader_message *in, const Volume* v);
static void load_volume_table();
static Volume* volume_for_path(const char* path);
static int parse_options(char* options, Volume* volume);

extern char* g_package_file;
static int num_volumes = 0;
static Volume* device_volumes = NULL;

// mount(fs_type, partition_type, location, mount_point)
//
//    fs_type="yaffs2" partition_type="MTD"     location=partition
//    fs_type="ext4"   partition_type="EMMC"    location=device
Value* MountFn(const char* name, State* state, int argc, Expr* argv[]) {
    char* result = NULL;
    if (argc != 4) {
        return ErrorAbort(state, "%s() expects 4 args, got %d", name, argc);
    }
    char* fs_type;
    char* partition_type;
    char* location;
    char* mount_point;
    if (ReadArgs(state, argv, 4, &fs_type, &partition_type,
                 &location, &mount_point) < 0) {
        return NULL;
    }

    if (strlen(fs_type) == 0) {
        ErrorAbort(state, "fs_type argument to %s() can't be empty", name);
        goto done;
    }
    if (strlen(partition_type) == 0) {
        ErrorAbort(state, "partition_type argument to %s() can't be empty",
                   name);
        goto done;
    }
    if (strlen(location) == 0) {
        ErrorAbort(state, "location argument to %s() can't be empty", name);
        goto done;
    }
    if (strlen(mount_point) == 0) {
        ErrorAbort(state, "mount_point argument to %s() can't be empty", name);
        goto done;
    }

#ifdef HAVE_SELINUX
    char *secontext = NULL;

    if (sehandle) {
        selabel_lookup(sehandle, &secontext, mount_point, 0755);
        setfscreatecon(secontext);
    }
#endif

    mkdir(mount_point, 0755);

#ifdef HAVE_SELINUX
    if (secontext) {
        freecon(secontext);
        setfscreatecon(NULL);
    }
#endif

    if (strcmp(partition_type, "MTD") == 0) {
        mtd_scan_partitions();
        const MtdPartition* mtd;
        mtd = mtd_find_partition_by_name(location);
        if (mtd == NULL) {
            fprintf(stderr, "%s: no mtd partition named \"%s\"",
                    name, location);
            result = strdup("");
            goto done;
        }
        if (mtd_mount_partition(mtd, mount_point, fs_type, 0 /* rw */) != 0) {
            fprintf(stderr, "mtd mount of %s failed: %s\n",
                    location, strerror(errno));
            result = strdup("");
            goto done;
        }
        result = mount_point;
    } else {
        if (mount(location, mount_point, fs_type,
                  MS_NOATIME | MS_NODEV | MS_NODIRATIME, "") < 0) {
            fprintf(stderr, "%s: failed to mount %s at %s: %s\n",
                    name, location, mount_point, strerror(errno));
            result = strdup("");
        } else {
            result = mount_point;
        }
    }

done:
    free(fs_type);
    free(partition_type);
    free(location);
    if (result != mount_point) free(mount_point);
    return StringValue(result);
}


// is_mounted(mount_point)
Value* IsMountedFn(const char* name, State* state, int argc, Expr* argv[]) {
    char* result = NULL;
    if (argc != 1) {
        return ErrorAbort(state, "%s() expects 1 arg, got %d", name, argc);
    }
    char* mount_point;
    if (ReadArgs(state, argv, 1, &mount_point) < 0) {
        return NULL;
    }
    if (strlen(mount_point) == 0) {
        ErrorAbort(state, "mount_point argument to unmount() can't be empty");
        goto done;
    }

    scan_mounted_volumes();
    const MountedVolume* vol = find_mounted_volume_by_mount_point(mount_point);
    if (vol == NULL) {
        result = strdup("");
    } else {
        result = mount_point;
    }

done:
    if (result != mount_point) free(mount_point);
    return StringValue(result);
}


Value* UnmountFn(const char* name, State* state, int argc, Expr* argv[]) {
    char* result = NULL;
    if (argc != 1) {
        return ErrorAbort(state, "%s() expects 1 arg, got %d", name, argc);
    }
    char* mount_point;
    if (ReadArgs(state, argv, 1, &mount_point) < 0) {
        return NULL;
    }
    if (strlen(mount_point) == 0) {
        ErrorAbort(state, "mount_point argument to unmount() can't be empty");
        goto done;
    }

    scan_mounted_volumes();
    const MountedVolume* vol = find_mounted_volume_by_mount_point(mount_point);
    if (vol == NULL) {
        fprintf(stderr, "unmount of %s failed; no such volume\n", mount_point);
        result = strdup("");
    } else {
        unmount_mounted_volume(vol);
        result = mount_point;
    }

done:
    if (result != mount_point) free(mount_point);
    return StringValue(result);
}


// format(fs_type, partition_type, location, fs_size, mount_point)
//
//    fs_type="yaffs2" partition_type="MTD"     location=partition fs_size=<bytes> mount_point=<location>
//    fs_type="ext4"   partition_type="EMMC"    location=device    fs_size=<bytes> mount_point=<location>
//    if fs_size == 0, then make_ext4fs uses the entire partition.
//    if fs_size > 0, that is the size to use
//    if fs_size < 0, then reserve that many bytes at the end of the partition
Value* FormatFn(const char* name, State* state, int argc, Expr* argv[]) {
    char* result = NULL;
    if (argc != 5) {
        return ErrorAbort(state, "%s() expects 5 args, got %d", name, argc);
    }
    char* fs_type;
    char* partition_type;
    char* location;
    char* fs_size;
    char* mount_point;

    if (ReadArgs(state, argv, 5, &fs_type, &partition_type, &location, &fs_size, &mount_point) < 0) {
        return NULL;
    }

    if (strlen(fs_type) == 0) {
        ErrorAbort(state, "fs_type argument to %s() can't be empty", name);
        goto done;
    }
    if (strlen(partition_type) == 0) {
        ErrorAbort(state, "partition_type argument to %s() can't be empty",
                   name);
        goto done;
    }
    if (strlen(location) == 0) {
        ErrorAbort(state, "location argument to %s() can't be empty", name);
        goto done;
    }

    if (strlen(mount_point) == 0) {
        ErrorAbort(state, "mount_point argument to %s() can't be empty", name);
        goto done;
    }

    if (strcmp(partition_type, "MTD") == 0) {
        mtd_scan_partitions();
        const MtdPartition* mtd = mtd_find_partition_by_name(location);
        if (mtd == NULL) {
            fprintf(stderr, "%s: no mtd partition named \"%s\"",
                    name, location);
            result = strdup("");
            goto done;
        }
        MtdWriteContext* ctx = mtd_write_partition(mtd);
        if (ctx == NULL) {
            fprintf(stderr, "%s: can't write \"%s\"", name, location);
            result = strdup("");
            goto done;
        }
        if (mtd_erase_blocks(ctx, -1) == -1) {
            mtd_write_close(ctx);
            fprintf(stderr, "%s: failed to erase \"%s\"", name, location);
            result = strdup("");
            goto done;
        }
        if (mtd_write_close(ctx) != 0) {
            fprintf(stderr, "%s: failed to close \"%s\"", name, location);
            result = strdup("");
            goto done;
        }
        result = location;
#ifdef USE_EXT4
    } else if (strcmp(fs_type, "ext4") == 0) {
        int status = make_ext4fs(location, atoll(fs_size), mount_point, sehandle);
        if (status != 0) {
            fprintf(stderr, "%s: make_ext4fs failed (%d) on %s",
                    name, status, location);
            result = strdup("");
            goto done;
        }
        result = location;
#endif
#if TARGET_BOARD_PLATFORM == rockchip
    } else if (strcmp(fs_type, "ext3") == 0) {
        int status = make_ext3fs(location);
        if (status != 0) {
            fprintf(stderr, "%s: make_ext3fs failed (%d) on %s",
                    name, status, location);
            result = strdup("");
            goto done;
        }
        result = location;
    } else if (strcmp(fs_type, "vfat") == 0) {
		char volume_label[16] = "\0";
		property_get("UserVolumeLabel", volume_label, "");
		printf("VolumeLabel: %s\n", volume_label);
		int status = make_vfat(location, volume_label);
		if (status != 0) {
			fprintf(stderr, "format_volume: make_vfat failed on %s\n", location);
			goto done;
		 }
		result = location;
#endif
    } else {
        fprintf(stderr, "%s: unsupported fs_type \"%s\" partition_type \"%s\"",
                name, fs_type, partition_type);
    }

done:
    free(fs_type);
    free(partition_type);
    if (result != location) free(location);
    return StringValue(result);
}


Value* DeleteFn(const char* name, State* state, int argc, Expr* argv[]) {
    char** paths = malloc(argc * sizeof(char*));
    int i;
    for (i = 0; i < argc; ++i) {
        paths[i] = Evaluate(state, argv[i]);
        if (paths[i] == NULL) {
            int j;
            for (j = 0; j < i; ++i) {
                free(paths[j]);
            }
            free(paths);
            return NULL;
        }
    }

    bool recursive = (strcmp(name, "delete_recursive") == 0);

    int success = 0;
    for (i = 0; i < argc; ++i) {
        if ((recursive ? dirUnlinkHierarchy(paths[i]) : unlink(paths[i])) == 0)
            ++success;
        free(paths[i]);
    }
    free(paths);

    char buffer[10];
    sprintf(buffer, "%d", success);
    return StringValue(strdup(buffer));
}


Value* ShowProgressFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc != 2) {
        return ErrorAbort(state, "%s() expects 2 args, got %d", name, argc);
    }
    char* frac_str;
    char* sec_str;
    if (ReadArgs(state, argv, 2, &frac_str, &sec_str) < 0) {
        return NULL;
    }

    double frac = strtod(frac_str, NULL);
    int sec = strtol(sec_str, NULL, 10);

    UpdaterInfo* ui = (UpdaterInfo*)(state->cookie);
    fprintf(ui->cmd_pipe, "progress %f %d\n", frac, sec);

    free(sec_str);
    return StringValue(frac_str);
}

Value* SetProgressFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc != 1) {
        return ErrorAbort(state, "%s() expects 1 arg, got %d", name, argc);
    }
    char* frac_str;
    if (ReadArgs(state, argv, 1, &frac_str) < 0) {
        return NULL;
    }

    double frac = strtod(frac_str, NULL);

    UpdaterInfo* ui = (UpdaterInfo*)(state->cookie);
    fprintf(ui->cmd_pipe, "set_progress %f\n", frac);

    return StringValue(frac_str);
}

// package_extract_dir(package_path, destination_path)
Value* PackageExtractDirFn(const char* name, State* state,
                          int argc, Expr* argv[]) {
    if (argc != 2) {
        return ErrorAbort(state, "%s() expects 2 args, got %d", name, argc);
    }
    char* zip_path;
    char* dest_path;
    if (ReadArgs(state, argv, 2, &zip_path, &dest_path) < 0) return NULL;

    ZipArchive* za = ((UpdaterInfo*)(state->cookie))->package_zip;

    // To create a consistent system image, never use the clock for timestamps.
    struct utimbuf timestamp = { 1217592000, 1217592000 };  // 8/1/2008 default

    bool success = mzExtractRecursive(za, zip_path, dest_path,
                                      MZ_EXTRACT_FILES_ONLY, &timestamp,
                                      NULL, NULL, sehandle);
    free(zip_path);
    free(dest_path);
    return StringValue(strdup(success ? "t" : ""));
}


// package_extract_file(package_path, destination_path)
//   or
// package_extract_file(package_path)
//   to return the entire contents of the file as the result of this
//   function (the char* returned is actually a FileContents*).
Value* PackageExtractFileFn(const char* name, State* state,
                           int argc, Expr* argv[]) {
    if (argc != 1 && argc != 2) {
        return ErrorAbort(state, "%s() expects 1 or 2 args, got %d",
                          name, argc);
    }
    bool success = false;
    if (argc == 2) {
        // The two-argument version extracts to a file.

        char* zip_path;
        char* dest_path;
        if (ReadArgs(state, argv, 2, &zip_path, &dest_path) < 0) return NULL;

        ZipArchive* za = ((UpdaterInfo*)(state->cookie))->package_zip;
        const ZipEntry* entry = mzFindZipEntry(za, zip_path);
        if (entry == NULL) {
            fprintf(stderr, "%s: no %s in package\n", name, zip_path);
            goto done2;
        }

        FILE* f = fopen(dest_path, "wb");
        if (f == NULL) {
            fprintf(stderr, "%s: can't open %s for write: %s\n",
                    name, dest_path, strerror(errno));
            goto done2;
        }
        success = mzExtractZipEntryToFile(za, entry, fileno(f));
        fclose(f);

      done2:
        free(zip_path);
        free(dest_path);
        return StringValue(strdup(success ? "t" : ""));
    } else {
        // The one-argument version returns the contents of the file
        // as the result.

        char* zip_path;
        Value* v = malloc(sizeof(Value));
        v->type = VAL_BLOB;
        v->size = -1;
        v->data = NULL;

        if (ReadArgs(state, argv, 1, &zip_path) < 0) return NULL;

        ZipArchive* za = ((UpdaterInfo*)(state->cookie))->package_zip;
        const ZipEntry* entry = mzFindZipEntry(za, zip_path);
        if (entry == NULL) {
            fprintf(stderr, "%s: no %s in package\n", name, zip_path);
            goto done1;
        }

        v->size = mzGetZipEntryUncompLen(entry);
        v->data = malloc(v->size);
        if (v->data == NULL) {
            fprintf(stderr, "%s: failed to allocate %ld bytes for %s\n",
                    name, (long)v->size, zip_path);
            goto done1;
        }

        success = mzExtractZipEntryToBuffer(za, entry,
                                            (unsigned char *)v->data);

      done1:
        free(zip_path);
        if (!success) {
            free(v->data);
            v->data = NULL;
            v->size = -1;
        }
        return v;
    }
}


// symlink target src1 src2 ...
//    unlinks any previously existing src1, src2, etc before creating symlinks.
Value* SymlinkFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc == 0) {
        return ErrorAbort(state, "%s() expects 1+ args, got %d", name, argc);
    }
    char* target;
    target = Evaluate(state, argv[0]);
    if (target == NULL) return NULL;

    char** srcs = ReadVarArgs(state, argc-1, argv+1);
    if (srcs == NULL) {
        free(target);
        return NULL;
    }

    int bad = 0;
    int i;
    for (i = 0; i < argc-1; ++i) {
        if (unlink(srcs[i]) < 0) {
            if (errno != ENOENT) {
                fprintf(stderr, "%s: failed to remove %s: %s\n",
                        name, srcs[i], strerror(errno));
                ++bad;
            }
        }
        if (symlink(target, srcs[i]) < 0) {
            fprintf(stderr, "%s: failed to symlink %s to %s: %s\n",
                    name, srcs[i], target, strerror(errno));
            ++bad;
        }
        free(srcs[i]);
    }
    free(srcs);
    if (bad) {
        return ErrorAbort(state, "%s: some symlinks failed", name);
    }
    return StringValue(strdup(""));
}


Value* SetPermFn(const char* name, State* state, int argc, Expr* argv[]) {
    char* result = NULL;
    bool recursive = (strcmp(name, "set_perm_recursive") == 0);

    int min_args = 4 + (recursive ? 1 : 0);
    if (argc < min_args) {
        return ErrorAbort(state, "%s() expects %d+ args, got %d", name, argc);
    }

    char** args = ReadVarArgs(state, argc, argv);
    if (args == NULL) return NULL;

    char* end;
    int i;
    int bad = 0;

    int uid = strtoul(args[0], &end, 0);
    if (*end != '\0' || args[0][0] == 0) {
        ErrorAbort(state, "%s: \"%s\" not a valid uid", name, args[0]);
        goto done;
    }

    int gid = strtoul(args[1], &end, 0);
    if (*end != '\0' || args[1][0] == 0) {
        ErrorAbort(state, "%s: \"%s\" not a valid gid", name, args[1]);
        goto done;
    }

    if (recursive) {
        int dir_mode = strtoul(args[2], &end, 0);
        if (*end != '\0' || args[2][0] == 0) {
            ErrorAbort(state, "%s: \"%s\" not a valid dirmode", name, args[2]);
            goto done;
        }

        int file_mode = strtoul(args[3], &end, 0);
        if (*end != '\0' || args[3][0] == 0) {
            ErrorAbort(state, "%s: \"%s\" not a valid filemode",
                       name, args[3]);
            goto done;
        }

        for (i = 4; i < argc; ++i) {
            dirSetHierarchyPermissions(args[i], uid, gid, dir_mode, file_mode);
        }
    } else {
        int mode = strtoul(args[2], &end, 0);
        if (*end != '\0' || args[2][0] == 0) {
            ErrorAbort(state, "%s: \"%s\" not a valid mode", name, args[2]);
            goto done;
        }

        for (i = 3; i < argc; ++i) {
            if (chown(args[i], uid, gid) < 0) {
                fprintf(stderr, "%s: chown of %s to %d %d failed: %s\n",
                        name, args[i], uid, gid, strerror(errno));
                ++bad;
            }
            if (chmod(args[i], mode) < 0) {
                fprintf(stderr, "%s: chmod of %s to %o failed: %s\n",
                        name, args[i], mode, strerror(errno));
                ++bad;
            }
        }
    }
    result = strdup("");

done:
    for (i = 0; i < argc; ++i) {
        free(args[i]);
    }
    free(args);

    if (bad) {
        free(result);
        return ErrorAbort(state, "%s: some changes failed", name);
    }
    return StringValue(result);
}


Value* GetPropFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc != 1) {
        return ErrorAbort(state, "%s() expects 1 arg, got %d", name, argc);
    }
    char* key;
    key = Evaluate(state, argv[0]);
    if (key == NULL) return NULL;

    char value[PROPERTY_VALUE_MAX];
    property_get(key, value, "");
    free(key);

    return StringValue(strdup(value));
}


// file_getprop(file, key)
//
//   interprets 'file' as a getprop-style file (key=value pairs, one
//   per line, # comment lines and blank lines okay), and returns the value
//   for 'key' (or "" if it isn't defined).
Value* FileGetPropFn(const char* name, State* state, int argc, Expr* argv[]) {
    char* result = NULL;
    char* buffer = NULL;
    char* filename;
    char* key;
    if (ReadArgs(state, argv, 2, &filename, &key) < 0) {
        return NULL;
    }

    struct stat st;
    if (stat(filename, &st) < 0) {
        ErrorAbort(state, "%s: failed to stat \"%s\": %s",
                   name, filename, strerror(errno));
        goto done;
    }

#define MAX_FILE_GETPROP_SIZE    65536

    if (st.st_size > MAX_FILE_GETPROP_SIZE) {
        ErrorAbort(state, "%s too large for %s (max %d)",
                   filename, name, MAX_FILE_GETPROP_SIZE);
        goto done;
    }

    buffer = malloc(st.st_size+1);
    if (buffer == NULL) {
        ErrorAbort(state, "%s: failed to alloc %d bytes", name, st.st_size+1);
        goto done;
    }

    FILE* f = fopen(filename, "rb");
    if (f == NULL) {
        ErrorAbort(state, "%s: failed to open %s: %s",
                   name, filename, strerror(errno));
        goto done;
    }

    if (fread(buffer, 1, st.st_size, f) != st.st_size) {
        ErrorAbort(state, "%s: failed to read %d bytes from %s",
                   name, st.st_size+1, filename);
        fclose(f);
        goto done;
    }
    buffer[st.st_size] = '\0';

    fclose(f);

    char* line = strtok(buffer, "\n");
    do {
        // skip whitespace at start of line
        while (*line && isspace(*line)) ++line;

        // comment or blank line: skip to next line
        if (*line == '\0' || *line == '#') continue;

        char* equal = strchr(line, '=');
        if (equal == NULL) {
            ErrorAbort(state, "%s: malformed line \"%s\": %s not a prop file?",
                       name, line, filename);
            goto done;
        }

        // trim whitespace between key and '='
        char* key_end = equal-1;
        while (key_end > line && isspace(*key_end)) --key_end;
        key_end[1] = '\0';

        // not the key we're looking for
        if (strcmp(key, line) != 0) continue;

        // skip whitespace after the '=' to the start of the value
        char* val_start = equal+1;
        while(*val_start && isspace(*val_start)) ++val_start;

        // trim trailing whitespace
        char* val_end = val_start + strlen(val_start)-1;
        while (val_end > val_start && isspace(*val_end)) --val_end;
        val_end[1] = '\0';

        result = strdup(val_start);
        break;

    } while ((line = strtok(NULL, "\n")));

    if (result == NULL) result = strdup("");

  done:
    free(filename);
    free(key);
    free(buffer);
    return StringValue(result);
}


static bool write_raw_image_cb(const unsigned char* data,
                               int data_len, void* ctx) {
    int r = mtd_write_data((MtdWriteContext*)ctx, (const char *)data, data_len);
    if (r == data_len) return true;
    fprintf(stderr, "%s\n", strerror(errno));
    return false;
}

// write_raw_image(filename_or_blob, partition)
Value* WriteRawImageFn(const char* name, State* state, int argc, Expr* argv[]) {
    char* result = NULL;

    Value* partition_value;
    Value* contents;
    if (ReadValueArgs(state, argv, 2, &contents, &partition_value) < 0) {
        return NULL;
    }

    char* partition = NULL;
    if (partition_value->type != VAL_STRING) {
        ErrorAbort(state, "partition argument to %s must be string", name);
        goto done;
    }
    partition = partition_value->data;
    if (strlen(partition) == 0) {
        ErrorAbort(state, "partition argument to %s can't be empty", name);
        goto done;
    }
    if (contents->type == VAL_STRING && strlen((char*) contents->data) == 0) {
        ErrorAbort(state, "file argument to %s can't be empty", name);
        goto done;
    }

    mtd_scan_partitions();
    const MtdPartition* mtd = mtd_find_partition_by_name(partition);
    if (mtd == NULL) {
        fprintf(stderr, "%s: no mtd partition named \"%s\"\n", name, partition);
        result = strdup("");
        goto done;
    }

    MtdWriteContext* ctx = mtd_write_partition(mtd);
    if (ctx == NULL) {
        fprintf(stderr, "%s: can't write mtd partition \"%s\"\n",
                name, partition);
        result = strdup("");
        goto done;
    }

    bool success;

    if (contents->type == VAL_STRING) {
        // we're given a filename as the contents
        char* filename = contents->data;
        FILE* f = fopen(filename, "rb");
        if (f == NULL) {
            fprintf(stderr, "%s: can't open %s: %s\n",
                    name, filename, strerror(errno));
            result = strdup("");
            goto done;
        }

        success = true;
        char* buffer = malloc(BUFSIZ);
        int read;
        while (success && (read = fread(buffer, 1, BUFSIZ, f)) > 0) {
            int wrote = mtd_write_data(ctx, buffer, read);
            success = success && (wrote == read);
        }
        free(buffer);
        fclose(f);
    } else {
        // we're given a blob as the contents
        ssize_t wrote = mtd_write_data(ctx, contents->data, contents->size);
        success = (wrote == contents->size);
    }
    if (!success) {
        fprintf(stderr, "mtd_write_data to %s failed: %s\n",
                partition, strerror(errno));
    }

    if (mtd_erase_blocks(ctx, -1) == -1) {
        fprintf(stderr, "%s: error erasing blocks of %s\n", name, partition);
    }
    if (mtd_write_close(ctx) != 0) {
        fprintf(stderr, "%s: error closing write of %s\n", name, partition);
    }

    printf("%s %s partition\n",
           success ? "wrote" : "failed to write", partition);

    result = success ? partition : strdup("");

done:
    if (result != partition) FreeValue(partition_value);
    FreeValue(contents);
    return StringValue(result);
}

//add by mmk@rock-chips.com
//update the parameter partition
Value* WriteRawParameterImageFn(const char* name, State* state, int argc, Expr* argv[]) {
    char* result = NULL;

    Value* partition_value;
    Value* contents;
    if (ReadValueArgs(state, argv, 2, &contents, &partition_value) < 0) {
        return NULL;
    }

    if (partition_value->type != VAL_STRING) {
        ErrorAbort(state, "partition argument to %s must be string", name);
        goto done;
    }
    char* partition = partition_value->data;
    if (strlen(partition) == 0) {
        ErrorAbort(state, "partition argument to %s can't be empty", name);
        goto done;
    }

    if (strcmp(partition, "parameter")) {
    	ErrorAbort(state, "partition argument to %s not be parameter", name);
        goto done;
    }

    if (contents->type == VAL_STRING) {
        ErrorAbort(state, "file argument to %s can't support", name);
        goto done;
    }

    mtd_scan_partitions();
    const MtdPartition* mtd = mtd_find_partition_by_name(partition);
    if (mtd == NULL) {
        fprintf(stderr, "%s: no mtd partition named \"%s\"\n", name, partition);
        result = strdup("");
        goto done;
    }

    MtdReadContext* ctx_read = mtd_read_partition(mtd);
    if (ctx_read == NULL) {
    	fprintf(stderr, "%s: can't read mtd partition \"%s\"\n",
    			name, partition);
    	result = strdup("");
    	goto done;
    }

    printf("start compare parameter\n");
    //compare the new parameter and old parameter
    bool isParemeterSame = true;
    size_t once_len = 16*1024;
    size_t compared_len = 0;
    char* old_parameter_buf = malloc(once_len);
    while(compared_len < contents->size) {
    	memset(old_parameter_buf, 0, once_len);
    	size_t read = mtd_read_data(ctx_read, old_parameter_buf, once_len);
    	if(read != once_len) {
    		printf("read old_parameter error!\n");
    		result = strdup("");
    		mtd_read_close(ctx_read);
    		goto done;
    	}

    	size_t realCompareSize = ((compared_len + read) < contents->size) ? read : (contents->size - compared_len);
    	if(memcmp(contents->data + compared_len, old_parameter_buf, realCompareSize)) {
    		isParemeterSame = false;
    		break;
    	}

    	compared_len += read;
    }

    if(isParemeterSame) {
    	printf("parameter is same, not update!\n");
    	result = partition;
    	mtd_read_close(ctx_read);
    	goto done;
    }

    mtd_read_close(ctx_read);

    //update parameter
    MtdWriteContext* ctx = mtd_write_partition(mtd);
    if (ctx == NULL) {
        fprintf(stderr, "%s: can't write mtd partition \"%s\"\n",
                name, partition);
        result = strdup("");
        goto done;
    }

    bool success;

    // we're given a blob as the contents
    ssize_t wrote = mtd_write_data(ctx, contents->data, contents->size);
    success = (wrote == contents->size);

    if (!success) {
        fprintf(stderr, "mtd_write_data to %s failed: %s\n",
                partition, strerror(errno));
    }

    if (mtd_erase_blocks(ctx, -1) == -1) {
        fprintf(stderr, "%s: error erasing blocks of %s\n", name, partition);
    }
    if (mtd_write_close(ctx) != 0) {
        fprintf(stderr, "%s: error closing write of %s\n", name, partition);
    }

    result = success ? partition : strdup("");

    printf("the package path is %s\n", g_package_file);
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    strlcpy(boot.command, "boot-recovery", sizeof(boot.command));
    char cmd[100] = "recovery\n--update_package=";
    strcat(cmd, g_package_file);
    strlcpy(boot.recovery, cmd, sizeof(boot.recovery));
    set_bootloader_message(&boot);

    printf("update parameter success, reboot now...\n");
    android_reboot(ANDROID_RB_RESTART2, 0, "recovery");

done:
	free(old_parameter_buf);
    if (result != partition) FreeValue(partition_value);
    FreeValue(contents);
    return StringValue(result);
}

Value* ClearMiscCommandFn(const char* name, State* state, int argc, Expr* argv[]) {
	printf("clear misc command!\n");
	struct bootloader_message boot;
	memset(&boot, 0, sizeof(boot));
	set_bootloader_message(&boot);

	return StringValue(strdup(""));
}

static int parse_options(char* options, Volume* volume) {
    char* option;
    while (option = strtok(options, ",")) {
        options = NULL;

        if (strncmp(option, "length=", 7) == 0) {
            volume->length = strtoll(option+7, NULL, 10);
        } else {
        	printf("bad option \"%s\"\n", option);
            return -1;
        }
    }
    return 0;
}

static void load_volume_table() {
    int alloc = 2;
    device_volumes = malloc(alloc * sizeof(Volume));

    // Insert an entry for /tmp, which is the ramdisk and is always mounted.
    device_volumes[0].mount_point = "/tmp";
    device_volumes[0].fs_type = "ramdisk";
    device_volumes[0].device = NULL;
    device_volumes[0].device2 = NULL;
    device_volumes[0].length = 0;
    num_volumes = 1;

    FILE* fstab = fopen("/etc/recovery.fstab", "r");
    if (fstab == NULL) {
    	printf("failed to open /etc/recovery.fstab (%s)\n", strerror(errno));
        return;
    }

    char buffer[1024];
    int i;
    while (fgets(buffer, sizeof(buffer)-1, fstab)) {
        for (i = 0; buffer[i] && isspace(buffer[i]); ++i);
        if (buffer[i] == '\0' || buffer[i] == '#') continue;

        char* original = strdup(buffer);

        char* mount_point = strtok(buffer+i, " \t\n");
        char* fs_type = strtok(NULL, " \t\n");
        char* device = strtok(NULL, " \t\n");
        // lines may optionally have a second device, to use if
        // mounting the first one fails.
        char* options = NULL;
        char* device2 = strtok(NULL, " \t\n");
        if (device2) {
            if (device2[0] == '/') {
                options = strtok(NULL, " \t\n");
            } else {
                options = device2;
                device2 = NULL;
            }
        }

        if (mount_point && fs_type && device) {
            while (num_volumes >= alloc) {
                alloc *= 2;
                device_volumes = realloc(device_volumes, alloc*sizeof(Volume));
            }
            device_volumes[num_volumes].mount_point = strdup(mount_point);
            device_volumes[num_volumes].fs_type = strdup(fs_type);
            device_volumes[num_volumes].device = strdup(device);
            device_volumes[num_volumes].device2 =
                device2 ? strdup(device2) : NULL;

            device_volumes[num_volumes].length = 0;
            if (parse_options(options, device_volumes + num_volumes) != 0) {
            	printf("skipping malformed recovery.fstab line: %s\n", original);
            } else {
                ++num_volumes;
            }
        } else {
        	printf("skipping malformed recovery.fstab line: %s\n", original);
        }
        free(original);
    }

    fclose(fstab);

    printf("recovery filesystem table\n");
    printf("=========================\n");
    for (i = 0; i < num_volumes; ++i) {
        Volume* v = &device_volumes[i];
        printf("  %d %s %s %s %s %lld\n", i, v->mount_point, v->fs_type,
               v->device, v->device2, v->length);
    }
    printf("\n");
}

static Volume* volume_for_path(const char* path) {
    int i;
    for (i = 0; i < num_volumes; ++i) {
        Volume* v = device_volumes+i;
        int len = strlen(v->mount_point);
        if (strncmp(path, v->mount_point, len) == 0 &&
            (path[len] == '\0' || path[len] == '/')) {
            return v;
        }
    }
    return NULL;
}

static int set_bootloader_message(const struct bootloader_message *in) {
	if(device_volumes == NULL) {
		load_volume_table();
	}
    Volume* v = volume_for_path("/misc");
    if (v == NULL) {
      printf("Cannot load volume /misc!\n");
      return -1;
    }
    if (strcmp(v->fs_type, "mtd") == 0) {
        return set_bootloader_message_mtd(in, v);
    } else if (strcmp(v->fs_type, "emmc") == 0) {
    	return set_bootloader_message_block_rk29(in, v);
    }
    printf("unknown misc partition fs_type \"%s\"\n", v->fs_type);
    return -1;
}

static const int MISC_PAGES = 3;         // number of pages to save
static const int MISC_COMMAND_PAGE = 1;  // bootloader command is this page

static int set_bootloader_message_mtd(const struct bootloader_message *in,
                                      const Volume* v) {
    size_t write_size;
    mtd_scan_partitions();
    const MtdPartition *part = mtd_find_partition_by_name(v->device);
    if (part == NULL || mtd_partition_info(part, NULL, NULL, &write_size)) {
    	printf("Can't find %s\n", v->device);
        return -1;
    }

    MtdReadContext *read = mtd_read_partition(part);
    if (read == NULL) {
    	printf("Can't open %s\n(%s)\n", v->device, strerror(errno));
        return -1;
    }

    ssize_t size = write_size * MISC_PAGES;
    char data[size];
    ssize_t r = mtd_read_data(read, data, size);
    if (r != size) printf("Can't read %s\n(%s)\n", v->device, strerror(errno));
    mtd_read_close(read);
    if (r != size) return -1;

    memcpy(&data[write_size * MISC_COMMAND_PAGE], in, sizeof(*in));

    MtdWriteContext *write = mtd_write_partition(part);
    if (write == NULL) {
    	printf("Can't open %s\n(%s)\n", v->device, strerror(errno));
        return -1;
    }
    if (mtd_write_data(write, data, size) != size) {
    	printf("Can't write %s\n(%s)\n", v->device, strerror(errno));
        mtd_write_close(write);
        return -1;
    }
    if (mtd_write_close(write)) {
    	printf("Can't finish %s\n(%s)\n", v->device, strerror(errno));
        return -1;
    }

    printf("Set boot command \"%s\"\n", in->command[0] != 255 ? in->command : "");
    return 0;
}

static int set_bootloader_message_block_rk29(const struct bootloader_message *in,
                                        const Volume* v) {

    FILE* f = fopen(v->device, "wb+");

    if (f == NULL) {
    	printf("Can't open %s\n(%s)\n", v->device, strerror(errno));
        return -1;
    }

    const ssize_t size =WRITE_SIZE * MISC_PAGES;
    char data[size];

    memset(data,0,size);
    memcpy(&data[WRITE_SIZE * MISC_COMMAND_PAGE], in, sizeof(*in));

    int count = rk29_fwrite(data, size, 1, f);

    if (count != 1) {
    	printf("Failed writing %s\n(%s)\n", v->device, strerror(errno));
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0) {
    	printf("Failed closing %s\n(%s)\n", v->device, strerror(errno));
        return -1;
    }
    return 0;
}

// apply_patch_space(bytes)
Value* ApplyPatchSpaceFn(const char* name, State* state,
                         int argc, Expr* argv[]) {
    char* bytes_str;
    if (ReadArgs(state, argv, 1, &bytes_str) < 0) {
        return NULL;
    }

    char* endptr;
    size_t bytes = strtol(bytes_str, &endptr, 10);
    if (bytes == 0 && endptr == bytes_str) {
        ErrorAbort(state, "%s(): can't parse \"%s\" as byte count\n\n",
                   name, bytes_str);
        free(bytes_str);
        return NULL;
    }

    return StringValue(strdup(CacheSizeCheck(bytes) ? "" : "t"));
}


// apply_patch(srcfile, tgtfile, tgtsha1, tgtsize, sha1_1, patch_1, ...)
Value* ApplyPatchFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc < 6 || (argc % 2) == 1) {
        return ErrorAbort(state, "%s(): expected at least 6 args and an "
                                 "even number, got %d",
                          name, argc);
    }

    char* source_filename;
    char* target_filename;
    char* target_sha1;
    char* target_size_str;
    if (ReadArgs(state, argv, 4, &source_filename, &target_filename,
                 &target_sha1, &target_size_str) < 0) {
        return NULL;
    }

    char* endptr;
    size_t target_size = strtol(target_size_str, &endptr, 10);
    if (target_size == 0 && endptr == target_size_str) {
        ErrorAbort(state, "%s(): can't parse \"%s\" as byte count",
                   name, target_size_str);
        free(source_filename);
        free(target_filename);
        free(target_sha1);
        free(target_size_str);
        return NULL;
    }

    int patchcount = (argc-4) / 2;
    Value** patches = ReadValueVarArgs(state, argc-4, argv+4);

    int i;
    for (i = 0; i < patchcount; ++i) {
        if (patches[i*2]->type != VAL_STRING) {
            ErrorAbort(state, "%s(): sha-1 #%d is not string", name, i);
            break;
        }
        if (patches[i*2+1]->type != VAL_BLOB) {
            ErrorAbort(state, "%s(): patch #%d is not blob", name, i);
            break;
        }
    }
    if (i != patchcount) {
        for (i = 0; i < patchcount*2; ++i) {
            FreeValue(patches[i]);
        }
        free(patches);
        return NULL;
    }

    char** patch_sha_str = malloc(patchcount * sizeof(char*));
    for (i = 0; i < patchcount; ++i) {
        patch_sha_str[i] = patches[i*2]->data;
        patches[i*2]->data = NULL;
        FreeValue(patches[i*2]);
        patches[i] = patches[i*2+1];
    }

    int result = applypatch(source_filename, target_filename,
                            target_sha1, target_size,
                            patchcount, patch_sha_str, patches);

    for (i = 0; i < patchcount; ++i) {
        FreeValue(patches[i]);
    }
    free(patch_sha_str);
    free(patches);

    return StringValue(strdup(result == 0 ? "t" : ""));
}

// apply_patch_check(file, [sha1_1, ...])
Value* ApplyPatchCheckFn(const char* name, State* state,
                         int argc, Expr* argv[]) {
    if (argc < 1) {
        return ErrorAbort(state, "%s(): expected at least 1 arg, got %d",
                          name, argc);
    }

    char* filename;
    if (ReadArgs(state, argv, 1, &filename) < 0) {
        return NULL;
    }

    int patchcount = argc-1;
    char** sha1s = ReadVarArgs(state, argc-1, argv+1);

    int result = applypatch_check(filename, patchcount, sha1s);

    int i;
    for (i = 0; i < patchcount; ++i) {
        free(sha1s[i]);
    }
    free(sha1s);

    return StringValue(strdup(result == 0 ? "t" : ""));
}

Value* UIPrintFn(const char* name, State* state, int argc, Expr* argv[]) {
    char** args = ReadVarArgs(state, argc, argv);
    if (args == NULL) {
        return NULL;
    }

    int size = 0;
    int i;
    for (i = 0; i < argc; ++i) {
        size += strlen(args[i]);
    }
    char* buffer = malloc(size+1);
    size = 0;
    for (i = 0; i < argc; ++i) {
        strcpy(buffer+size, args[i]);
        size += strlen(args[i]);
        free(args[i]);
    }
    free(args);
    buffer[size] = '\0';

    char* line = strtok(buffer, "\n");
    while (line) {
        fprintf(((UpdaterInfo*)(state->cookie))->cmd_pipe,
                "ui_print %s\n", line);
        line = strtok(NULL, "\n");
    }
    fprintf(((UpdaterInfo*)(state->cookie))->cmd_pipe, "ui_print\n");

    return StringValue(buffer);
}

Value* WipeCacheFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc != 0) {
        return ErrorAbort(state, "%s() expects no args, got %d", name, argc);
    }
    fprintf(((UpdaterInfo*)(state->cookie))->cmd_pipe, "wipe_cache\n");
    return StringValue(strdup("t"));
}

Value* RunProgramFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc < 1) {
        return ErrorAbort(state, "%s() expects at least 1 arg", name);
    }
    char** args = ReadVarArgs(state, argc, argv);
    if (args == NULL) {
        return NULL;
    }

    char** args2 = malloc(sizeof(char*) * (argc+1));
    memcpy(args2, args, sizeof(char*) * argc);
    args2[argc] = NULL;

    fprintf(stderr, "about to run program [%s] with %d args\n", args2[0], argc);

    pid_t child = fork();
    if (child == 0) {
        execv(args2[0], args2);
        fprintf(stderr, "run_program: execv failed: %s\n", strerror(errno));
        _exit(1);
    }
    int status;
    waitpid(child, &status, 0);
    if (WIFEXITED(status)) {
        if (WEXITSTATUS(status) != 0) {
            fprintf(stderr, "run_program: child exited with status %d\n",
                    WEXITSTATUS(status));
        }
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "run_program: child terminated by signal %d\n",
                WTERMSIG(status));
    }

    int i;
    for (i = 0; i < argc; ++i) {
        free(args[i]);
    }
    free(args);
    free(args2);

    char buffer[20];
    sprintf(buffer, "%d", status);

    return StringValue(strdup(buffer));
}

// Take a sha-1 digest and return it as a newly-allocated hex string.
static char* PrintSha1(uint8_t* digest) {
    char* buffer = malloc(SHA_DIGEST_SIZE*2 + 1);
    int i;
    const char* alphabet = "0123456789abcdef";
    for (i = 0; i < SHA_DIGEST_SIZE; ++i) {
        buffer[i*2] = alphabet[(digest[i] >> 4) & 0xf];
        buffer[i*2+1] = alphabet[digest[i] & 0xf];
    }
    buffer[i*2] = '\0';
    return buffer;
}

// sha1_check(data)
//    to return the sha1 of the data (given in the format returned by
//    read_file).
//
// sha1_check(data, sha1_hex, [sha1_hex, ...])
//    returns the sha1 of the file if it matches any of the hex
//    strings passed, or "" if it does not equal any of them.
//
Value* Sha1CheckFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc < 1) {
        return ErrorAbort(state, "%s() expects at least 1 arg", name);
    }

    Value** args = ReadValueVarArgs(state, argc, argv);
    if (args == NULL) {
        return NULL;
    }

    if (args[0]->size < 0) {
        fprintf(stderr, "%s(): no file contents received", name);
        return StringValue(strdup(""));
    }
    uint8_t digest[SHA_DIGEST_SIZE];
    SHA(args[0]->data, args[0]->size, digest);
    FreeValue(args[0]);

    if (argc == 1) {
        return StringValue(PrintSha1(digest));
    }

    int i;
    uint8_t* arg_digest = malloc(SHA_DIGEST_SIZE);
    for (i = 1; i < argc; ++i) {
        if (args[i]->type != VAL_STRING) {
            fprintf(stderr, "%s(): arg %d is not a string; skipping",
                    name, i);
        } else if (ParseSha1(args[i]->data, arg_digest) != 0) {
            // Warn about bad args and skip them.
            fprintf(stderr, "%s(): error parsing \"%s\" as sha-1; skipping",
                    name, args[i]->data);
        } else if (memcmp(digest, arg_digest, SHA_DIGEST_SIZE) == 0) {
            break;
        }
        FreeValue(args[i]);
    }
    if (i >= argc) {
        // Didn't match any of the hex strings; return false.
        return StringValue(strdup(""));
    }
    // Found a match; free all the remaining arguments and return the
    // matched one.
    int j;
    for (j = i+1; j < argc; ++j) {
        FreeValue(args[j]);
    }
    return args[i];
}

// Read a local file and return its contents (the Value* returned
// is actually a FileContents*).
Value* ReadFileFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc != 1) {
        return ErrorAbort(state, "%s() expects 1 arg, got %d", name, argc);
    }
    char* filename;
    if (ReadArgs(state, argv, 1, &filename) < 0) return NULL;

    Value* v = malloc(sizeof(Value));
    v->type = VAL_BLOB;

    FileContents fc;
    if (LoadFileContents(filename, &fc, RETOUCH_DONT_MASK) != 0) {
        ErrorAbort(state, "%s() loading \"%s\" failed: %s",
                   name, filename, strerror(errno));
        free(filename);
        free(v);
        free(fc.data);
        return NULL;
    }

    v->size = fc.size;
    v->data = (char*)fc.data;

    free(filename);
    return v;
}

void RegisterInstallFunctions() {
    RegisterFunction("mount", MountFn);
    RegisterFunction("is_mounted", IsMountedFn);
    RegisterFunction("unmount", UnmountFn);
    RegisterFunction("format", FormatFn);
    RegisterFunction("show_progress", ShowProgressFn);
    RegisterFunction("set_progress", SetProgressFn);
    RegisterFunction("delete", DeleteFn);
    RegisterFunction("delete_recursive", DeleteFn);
    RegisterFunction("package_extract_dir", PackageExtractDirFn);
    RegisterFunction("package_extract_file", PackageExtractFileFn);
    RegisterFunction("symlink", SymlinkFn);
    RegisterFunction("set_perm", SetPermFn);
    RegisterFunction("set_perm_recursive", SetPermFn);

    RegisterFunction("getprop", GetPropFn);
    RegisterFunction("file_getprop", FileGetPropFn);
    RegisterFunction("write_raw_image", WriteRawImageFn);
    RegisterFunction("write_raw_parameter_image", WriteRawParameterImageFn);
    RegisterFunction("clear_misc_command", ClearMiscCommandFn);

    RegisterFunction("apply_patch", ApplyPatchFn);
    RegisterFunction("apply_patch_check", ApplyPatchCheckFn);
    RegisterFunction("apply_patch_space", ApplyPatchSpaceFn);

    RegisterFunction("read_file", ReadFileFn);
    RegisterFunction("sha1_check", Sha1CheckFn);

    RegisterFunction("wipe_cache", WipeCacheFn);

    RegisterFunction("ui_print", UIPrintFn);

    RegisterFunction("run_program", RunProgramFn);
}
