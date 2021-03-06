/*
* Copyright (C) 2014 MediaTek Inc.
* Modification based on code covered by the mentioned copyright
* and/or permission notice(s).
*/

#include <stdio.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#if !defined(ARCH_X86)
#include <linux/mmc/sd_misc.h>
#endif
#include "bootloader.h"
#include "updater.h"
#include "mtdutils/mounts.h"
#include "mtdutils/mtdutils.h"
#include "mt_install.h"
#include "mt_gpt.h"
#include "mt_partition.h"
#include "minzip/DirUtil.h"

#ifdef MTK_SLC_BUFFER_SUPPORT
#include "sparse_format.h"
#endif

#ifdef USE_EXT4
#include "make_ext4fs.h"
#include "wipe.h"
#endif

#define PRELOADER_OFFSET_EMMC   (0x800)

/* external functions defined in install.c */
extern void uiPrintf(State* state, const char* format, ...);

static bool mt_is_mountable(const char *mount_point)
{
    return (
        !strcmp(mount_point, "/system") ||
        !strcmp(mount_point, "/data") ||
        !strcmp(mount_point, "/cache") ||
        !strcmp(mount_point, "/custom"));
}

#if defined(CACHE_MERGE_SUPPORT)
#include <dirent.h>

static const char *DATA_CACHE_ROOT = "/data/.cache";
static int need_clear_cache = 0;

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
int remove_dir(const char *dirname)
{
    DIR *dir;
    struct dirent *entry;
    char path[PATH_MAX];

    dir = opendir(dirname);
    if (dir == NULL) {
        LOGE("opendir %s failed\n", dirname);
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
            snprintf(path, (size_t) PATH_MAX, "%s/%s", dirname, entry->d_name);
            if (entry->d_type == DT_DIR) {
                remove_dir(path);
            } else {
                // delete file
                unlink(path);
            }
        }
    }
    closedir(dir);

    // now we can delete the empty dir
    rmdir(dirname);
    return 0;
}
#endif

//  cache merge init function
int mt_MountFn_cache_merge_init(char **mount_point, char **result)
{
#if defined(CACHE_MERGE_SUPPORT)
    if (!strcmp(*mount_point, "/cache")) {
        const MountedVolume* vol;

        scan_mounted_volumes();
        vol = find_mounted_volume_by_mount_point("/data");
        if (vol) {
            // create link if data is already mounted
            if (symlink(DATA_CACHE_ROOT, "/cache")) {
                if (errno != EEXIST) {
                    fprintf(stderr, "create symlink from %s to %s failed(%s)\n",
                                        DATA_CACHE_ROOT, "/cache", strerror(errno));
                    *result = strdup("");
                    return MT_FN_FAIL_EXIT;
                }
            }
            *result = strdup(*mount_point);
            return MT_FN_SUCCESS_EXIT;
        } else {
            // cache is under /data now, mount it
            *mount_point = "/data";
        }
    }
#endif
    return MT_FN_SUCCESS_CONTINUE;
}

int mt_MountFn_cache_merge_final(char **mount_point, char **result)
{
#if defined(CACHE_MERGE_SUPPORT)
    if (!strcmp(*mount_point, "/data")) {
        if (mkdir(DATA_CACHE_ROOT, 0770)) {
            if (errno != EEXIST) {
                fprintf(stderr, "mkdir %s error: %s\n", DATA_CACHE_ROOT, strerror(errno));
                *result = strdup("");
            } else if (need_clear_cache) {
                fprintf(stderr, "cache exists, clear it...\n");
                if (remove_dir(DATA_CACHE_ROOT)) {
                    fprintf(stderr, "remove_dir %s error: %s\n", DATA_CACHE_ROOT, strerror(errno));
                    result = strdup("");
                }
                if (mkdir(DATA_CACHE_ROOT, 0770) != 0) {
                    fprintf(stderr, "Can't mkdir %s (%s)\n", DATA_CACHE_ROOT, strerror(errno));
                    *result = strdup("");
                }
            }
        }
        if (symlink(DATA_CACHE_ROOT, "/cache")) {
            if (errno != EEXIST) {
                fprintf(stderr, "create symlink from %s to %s failed(%s)\n", DATA_CACHE_ROOT, "/cache", strerror(errno));
                *result = strdup("");
            }
        }
        need_clear_cache = 0;
    }
#endif
    return MT_FN_SUCCESS_CONTINUE;
}

// mount/attach ubifs/yaffs volumes
static int mt_MountFn_NAND(char **mount_point, char **result, char **fs_type, char **location,
    char **partition_type, const char *name, char* mount_options, bool has_mount_options, State* state)
{
#if defined(UBIFS_SUPPORT)
    //Attatch UBI device & Make UBI volum
    int n = -1;
    int ret;
    n = ubi_attach_mtd_user(*mount_point);

    if ((n != -1) && (n < 4)) {
      //attached successful, do nothing
    } else {
      ErrorAbort(state, "failed to attach %s\n", *location);
      return MT_FN_FAIL_EXIT;
    }

    //Mount UBI volume
    const unsigned long flags = MS_NOATIME | MS_NODEV | MS_NODIRATIME;
    char tmp[64];
    sprintf(tmp, "/dev/ubi%d_0", n);
    wait_for_file(tmp, 5);
    ret = mount(tmp, *mount_point, *fs_type, flags, "");
    if (ret < 0) {
      ubi_detach_dev(n);
      ErrorAbort(state, "failed to mount %s\n", *mount_point);
      *result = strdup("");
      return MT_FN_FAIL_EXIT;
    } else if (ret == 0) {
      *result = *mount_point;
    }
    //Volume  successfully mounted
    fprintf(stderr, "UBI mount successful %s\n", *mount_point);
#else
    free(*fs_type);
    *fs_type = strdup("yaffs2");
    free(*partition_type);
    *partition_type = strdup("MTD");

    mtd_scan_partitions();
    const MtdPartition* mtd;
    mtd = mtd_find_partition_by_name(*location);
    if (mtd == NULL) {
      uiPrintf(state, "%s: no mtd partition named \"%s\"",
               name, *location);
      *result = strdup("");
      return MT_FN_FAIL_EXIT;
    }
    if (mtd_mount_partition(mtd, *mount_point, *fs_type, 0 /* rw */) != 0) {
      uiPrintf(state, "mtd mount of %s failed: %s\n",
               *location, strerror(errno));
      *result = strdup("");
      return MT_FN_FAIL_EXIT;
    }
    *result = *mount_point;
#endif
    return MT_FN_SUCCESS_EXIT;
}
static int mt_MountFn_eMMC(char **mount_point, char **result, char **fs_type, char **location,
    char **partition_type, const char *name, char* mount_options, bool has_mount_options, State* state)
{
    *fs_type = strdup("ext4");
    free(*partition_type);
    *partition_type = strdup("EMMC");

    if (mount(*location, *mount_point, *fs_type, MS_NOATIME | MS_NODEV | MS_NODIRATIME, has_mount_options ? mount_options : "") < 0) {
        uiPrintf(state, "%s: failed to mount %s at %s: %s\n", name, *location, *mount_point, strerror(errno));
        *result = strdup("");
        return MT_FN_FAIL_EXIT;
    } else {
        *result = *mount_point;
    }
    return MT_FN_SUCCESS_EXIT;
}

//  Mount ext4/ubifs/yaffs volumes - /system, /data, /cache, /custom
int mt_MountFn(char **mount_point, char **result, char **fs_type, char **location,
    char **partition_type, const char *name, char* mount_options, bool has_mount_options, State* state)
{
    int ret = MT_FN_SUCCESS_CONTINUE;
    char *dev_path = NULL;
    if (mt_is_mountable(*mount_point)) {
        dev_path=get_partition_path(*location);
        free(*location);
        *location = dev_path;
        if (mt_get_phone_type() == NAND_TYPE) {
            ret=mt_MountFn_NAND(mount_point, result, fs_type, location,
                partition_type, name, mount_options, has_mount_options, state);
        } else {
            ret=mt_MountFn_eMMC(mount_point, result, fs_type, location,
                partition_type, name, mount_options, has_mount_options, state);
        }
    }
    return ret;
}

//  Unmount cache merge volumes
int mt_UnmountFn_chache_merege(char **mount_point, char **result)
{
#if defined(CACHE_MERGE_SUPPORT)
    if (!strcmp(*mount_point, "/cache")) {
        // remove cache link
        unlink(*mount_point);
        *result=*mount_point
        return MT_FN_SUCCESS_EXIT;
    }
#endif
    return MT_FN_SUCCESS_CONTINUE;
}

int mt_UnmountFn_ubifs(char *mount_point)
{
#if defined(UBIFS_SUPPORT)
    int ubi_num;

    if (!(!strcmp(mount_point, "/system") || !strcmp(mount_point, "/data") || !strcmp(mount_point, "/cache") || !strcmp(mount_point, "/.cache") || !strcmp(mount_point, "/custom"))) {
        LOGE("Invalid mount_point: %s\n", mount_point);
        return MT_FN_FAIL_EXIT;
    }

    if (!strcmp(mount_point, "/system")) {
        ubi_num = 0;
    }

    if (!strcmp(mount_point, "/data")) {
        ubi_num = 1;
    }

    if (!strcmp(mount_point, "/cache") || !strcmp(mount_point, "/.cache")) {
        ubi_num = 2;
    }

    if (!strcmp(mount_point, "/custom")) {
        //ubi_num = 3;
    }
    fprintf(stderr, "detaching ubi%d\n", ubi_num);
    if (ubi_detach_dev(ubi_num) == -1) {
        fprintf(stderr, "detaching ubi%d failed\n", ubi_num);
        return MT_FN_FAIL_CONTINUE;
    }
#endif
    return MT_FN_SUCCESS_CONTINUE;
}

//  Format ext4/ubifs/yaffs volumes - /system, /data, /cache, /custom
int mt_FormatFn(char **mount_point, char **result, char **fs_type, char **location,
    char **partition_type, const char *name, const char *fs_size)
{
    if (mt_is_mountable(*mount_point)) {
#if defined(CACHE_MERGE_SUPPORT)
        if (!strcmp(*mount_point, "/cache")) {
            const MountedVolume* vol;

            // set flag if /data is already unmounted
            // cache will be cleared after data mounted
            scan_mounted_volumes();
            vol = find_mounted_volume_by_mount_point("/data");
            if (vol == NULL) {
                fprintf(stderr, "/data is unmounted before formatting cache!\n");
                need_clear_cache = 1;
            } else {
                if (remove_dir(DATA_CACHE_ROOT)) {
                    fprintf(stderr, "remove_dir %s error: %s\n", DATA_CACHE_ROOT, strerror(errno));
                    *result = strdup("");
                    return MT_FN_FAIL_EXIT;
                }
                if (mkdir(DATA_CACHE_ROOT, 0770) != 0) {
                    fprintf(stderr, "Can't mkdir %s (%s)\n", DATA_CACHE_ROOT, strerror(errno));
                    *result = strdup("");
                    return MT_FN_FAIL_EXIT;
                }
                fprintf(stderr, "format cache successfully!\n");
            }
            *result = strdup(dev[CACHE_INDEX]);
            return MT_FN_SUCCESS_EXIT;
        }
#endif
        // wschen 2014-02-21
        // call umount first, this will prevent last time FULL OTA upgrade fail, if already mount /system
        if (umount(*mount_point) == -1) {
            fprintf(stderr, "umount %s fail(%s)\n", *mount_point, strerror(errno));
        }

        char *dev_path=get_partition_path(*location);
        free(*location);
        *location = dev_path;
        if (mt_get_phone_type() == NAND_TYPE) {
#if defined(UBIFS_SUPPORT)
            int ret;
            ret = ubi_format(*mount_point);

            if (ret != 0) {
                fprintf(stderr, "%s: no mtd partition named \"%s\"", name, *location);
                *result = strdup("");
                return MT_FN_FAIL_EXIT;
            } else {
                *result = *location;
            }

#else
            mtd_scan_partitions();
            const MtdPartition* mtd = mtd_find_partition_by_name(*location);
            if (mtd == NULL) {
                fprintf(stderr, "%s: no mtd partition named \"%s\"", name, *location);
                *result = strdup("");
                return MT_FN_FAIL_EXIT;
            }
            MtdWriteContext* ctx = mtd_write_partition(mtd);
            if (ctx == NULL) {
                fprintf(stderr, "%s: can't write \"%s\"", name, *location);
                *result = strdup("");
                return MT_FN_FAIL_EXIT;
            }
            if (mtd_erase_blocks(ctx, -1) == (off64_t)-1) {
                mtd_write_close(ctx);
                fprintf(stderr, "%s: failed to erase \"%s\"", name, *location);
                *result = strdup("");
                return MT_FN_FAIL_EXIT;
            }
            if (mtd_write_close(ctx) != 0) {
                fprintf(stderr, "%s: failed to close \"%s\"", name, *location);
                *result = strdup("");
                return MT_FN_FAIL_EXIT;
            }
            *result = *location;
#endif
        }

#ifdef USE_EXT4
        else {
            int status = make_ext4fs(*location, atoll(fs_size), *mount_point, sehandle);
            if (status != 0) {
                fprintf(stderr, "%s: make_ext4fs failed (%d) on %s", name, status, *location);
                *result = strdup("");
                return MT_FN_FAIL_EXIT;
            }
            *result = *location;
        }
#endif
    } else {
        if (strcmp(*partition_type, "MTD") == 0) {
            mtd_scan_partitions();
            const MtdPartition* mtd = mtd_find_partition_by_name(*location);
            if (mtd == NULL) {
                fprintf(stderr, "%s: no mtd partition named \"%s\"", name, *location);
                *result = strdup("");
                return MT_FN_FAIL_EXIT;
            }
            MtdWriteContext* ctx = mtd_write_partition(mtd);
            if (ctx == NULL) {
                fprintf(stderr, "%s: can't write \"%s\"", name, *location);
                *result = strdup("");
                return MT_FN_FAIL_EXIT;
            }
            if (mtd_erase_blocks(ctx, -1) == (off64_t)-1) {
                mtd_write_close(ctx);
                fprintf(stderr, "%s: failed to erase \"%s\"", name, *location);
                *result = strdup("");
                return MT_FN_FAIL_EXIT;
            }
            if (mtd_write_close(ctx) != 0) {
                fprintf(stderr, "%s: failed to close \"%s\"", name, *location);
                *result = strdup("");
                return MT_FN_FAIL_EXIT;
            }
            *result = *location;
#ifdef USE_EXT4
        } else if (strcmp(*fs_type, "ext4") == 0) {
            int status = make_ext4fs(*location, atoll(fs_size), *mount_point, sehandle);
            if (status != 0) {
                fprintf(stderr, "%s: make_ext4fs failed (%d) on %s", name, status, *location);
                *result = strdup("");
                return MT_FN_FAIL_EXIT;
            }
            *result = *location;
#endif
        } else {
            fprintf(stderr, "%s: unsupported fs_type \"%s\" partition_type \"%s\"", name, *fs_type, *partition_type);
        }
    }
    return MT_FN_SUCCESS_CONTINUE;
}

Value* mtGetUpdateStageFn(const char* name, State* state, int argc, Expr* argv[]) {
    char buf[64];
    char* filename;
    UpdaterInfo* ui = (UpdaterInfo*)(state->cookie);
    if (ReadArgs(state, argv, 1, &filename) < 0) return NULL;
    FILE *fp = fopen(filename, "r");
    strcpy(buf, "0");
    if (fp) {
        fgets(buf, sizeof(buf), fp);
        fclose(fp);
    }
    free(filename);
    return StringValue(strdup(buf));
}

Value* mtSetUpdateStageFn(const char* name, State* state, int argc, Expr* argv[]) {
    char *buf;
    char* filename;
    UpdaterInfo* ui = (UpdaterInfo*)(state->cookie);
    if (ReadArgs(state, argv, 2, &filename, &buf) < 0) return NULL;
    FILE *fp = fopen(filename, "w+");
    if (fp) {
        fputs(buf, fp);
        fclose(fp);
    }
    free(buf);
    return StringValue(filename);
}

static int mt_update_erase_part(State* state, char *partition_name) {
    char* dev_name = get_partition_path(partition_name);

    int fd = open(dev_name, O_WRONLY | O_SYNC);
    if (fd != -1) {
        char *buf = (char *)malloc(1024);
        memset(buf, 0, 1024);
        if (write(fd, buf, 1024) == -1) {
            fprintf(stderr, "write to %s fail\n", dev_name);
            close(fd);
            free(dev_name);
            free(buf);
            return 1;
        }
        printf("write done\n");
        close(fd);
        free(buf);
    } else {
        fprintf(stderr, "open %s fail\n", dev_name);
        free(dev_name);
        return 1;
    }
    free(dev_name);
    return 0;
}

static int mt_update_active_part(State* state, char* from_partition, char *to_partition)
{
    if (!strncmp(from_partition, "tee", strlen("tee"))          // tee1 and tee2 partition not use active bit
        || ((mt_get_phone_type() == NAND_TYPE) && (!strncasecmp(from_partition, "preloader", strlen("preloader"))))  // preloader on NAND not use active bit
        ) {
        if (!strcmp(from_partition, "tee1") || !strcasecmp(from_partition, "preloader")) // only do erase when main partition to alt partition
            return mt_update_erase_part(state, from_partition);
    } else if ((mt_get_phone_type() == EMMC_TYPE) && !strncasecmp(from_partition, "preloader", strlen("preloader"))) { // preloader on EMMC just switch register
        struct msdc_ioctl st_ioctl_arg;
        unsigned int bootpart = 0;
        int fd = open("/dev/misc-sd", O_RDWR);
        if (fd >= 0) {
            memset(&st_ioctl_arg,0,sizeof(struct msdc_ioctl));
            st_ioctl_arg.host_num = 0;
            st_ioctl_arg.opcode = MSDC_SET_BOOTPART;
            st_ioctl_arg.total_size = 1;
            if (!strcmp(to_partition, "preloader"))
                bootpart = EMMC_BOOT1_EN;
            else
                bootpart = EMMC_BOOT2_EN;
            st_ioctl_arg.buffer = &bootpart;
            int ret = ioctl(fd, MSDC_SET_BOOTPART, &st_ioctl_arg);
            if (ret < 0)
                printf("set boot_part fail: %s\n", strerror(errno));
            printf("switch bootpart to  = %d, ret = %d\n", bootpart, ret);
            close(fd);
        } else {
            uiPrintf(state, "set boot part fail, can not open misc-sd\n");
        }
    } else if (support_gpt()) {
        // need to set to_partition active bit to 1 and then set from_partition active bit to 0
        int ret = mt_gpt_update_active_part(to_partition, 1) | mt_gpt_update_active_part(from_partition, 0);
        return ret;
    } else {
        // TODO: pmt type active bit switch
    }
    return 1;
}

Value* mtShowUpdateStageFn(const char* name, State* state, int argc, Expr* argv[]) {
    Value* retval = mtGetUpdateStageFn(name, state, argc, argv);
    printf("Current Stage is %s\n", retval->data);
    return retval;
}

Value* mtSwitchActiveFn(const char* name, State* state, int argc, Expr* argv[]) {
    char* from_partition;   // partition active bit set to 0
    char* to_partition;     // partition active bit set to 1
    if (argc != 2)
        return ErrorAbort(state, "%s() expects 2 arg, got %d", name, argc);
    if (ReadArgs(state, argv, 2, &from_partition, &to_partition) < 0) return NULL;

    mt_update_active_part(state, from_partition, to_partition);
    printf("Switch %s active to %s\n", from_partition, to_partition);
    free(to_partition);
    return StringValue(from_partition);
}

#ifdef MTK_SLC_BUFFER_SUPPORT
static void show_sparse_header(sparse_header_t *sparse_header)
{
   printf("=== Sparse Image Header ===\n");
   printf("Read Sparse Image Header = %d \n",sizeof(sparse_header_t));
   printf("magic: 0x%x\n", sparse_header->magic);
   printf("major_version: 0x%x\n", sparse_header->major_version);
   printf("minor_version: 0x%x\n", sparse_header->minor_version);
   printf("file_hdr_sz: %d\n", sparse_header->file_hdr_sz);
   printf("chunk_hdr_sz: %d\n", sparse_header->chunk_hdr_sz);
   printf("blk_sz: %d\n", sparse_header->blk_sz);
   printf("total_blks: %d\n", sparse_header->total_blks);
}

static void show_chunk_header(chunk_header_t *chunk_header)
{
   printf("=== Chunk Header ===\n");
   printf("Read Chunk Header = %d \n",sizeof(chunk_header_t));
   printf("chunk_type: 0x%x\n", chunk_header->chunk_type);
   printf("chunk_sz: %d\n", chunk_header->chunk_sz);
   printf("total_size: 0x%x\n", chunk_header->total_sz);
}


static bool receive_new_sparse_data(const unsigned char* data, int size, void* cookie) {
    int* fd = (int*) cookie;
    static char *tmp_buf = NULL , *tmp_buf_head = NULL;
    static int state = HANDL_SPARESE_INIT_STATE;
    static int blk_remain = 0, blk_size = 0, total_chunk = 0, total_blks = 0, data_remain = 0;
    static int chunk_hdr_size = 0;;
    int copy_size = 0 ,written = 0, input_size = 0;

    input_size = size;

    while(1)
    {
        /* 1. Handle Sparse and 1st chunk header */
        if(state == HANDL_SPARESE_INIT_STATE) {
            sparse_header_t* sparse_header = (sparse_header_t*) data;  /* Assign sparse header */
            if(sparse_header->magic != SPARSE_HEADER_MAGIC)
            {
               printf("First data must be sparse header\n");
               if(tmp_buf_head)  free(tmp_buf_head);
               return false;
            }
            else
            {
               printf("Data stream has a sparse header block size = %d\n", sparse_header->blk_sz);

               tmp_buf = (char *)malloc(sparse_header->blk_sz); /* Malloc block size temp buffer */
               tmp_buf_head = tmp_buf;

               if(tmp_buf == NULL)  {
                  printf("receive_new_sparse_data malloc error \n");
                  if(tmp_buf_head)  free(tmp_buf_head);
                  return false;
               }

                // show_sparse_header(sparse_header);

                /* Skip spares header data and size */
                data += sparse_header->file_hdr_sz;
                size -= sparse_header->file_hdr_sz;

                total_blks = sparse_header->total_blks;
                blk_remain = sparse_header->blk_sz;
                blk_size = sparse_header->blk_sz;
                chunk_hdr_size = sparse_header->chunk_hdr_sz;
                state = HANDLE_CHUNK_HEADER;
            }
        }

        /* 2. Handle Chunk header */
        if(state == HANDLE_CHUNK_HEADER)
        {
            chunk_header_t* chunk_header = (chunk_header_t*) data;  /* Assign chunk header */
            //printf("chunk header\n");
            //show_chunk_header(chunk_header);

            /* Skip chunk header data and size */
            data += chunk_hdr_size;
            size -= chunk_hdr_size;

            /* The number of total chunk write */
            total_chunk = chunk_header->chunk_sz;

            if(chunk_header->chunk_type == CHUNK_TYPE_RAW)
            {
                //printf("CHUNK_TYPE_RAW\n");
                state = PREPARE_BLOCK_SIZE_BUFFER;
            }
            else if(chunk_header->chunk_type == CHUNK_TYPE_DONT_CARE)
            {
                //printf("CHUNK_TYPE_DONT_CARE\n");
                if(TEMP_FAILURE_RETRY(lseek(*fd, total_chunk*blk_size, SEEK_CUR) == -1)) {
                    printf("lseek error %s", strerror(errno));
                    if(tmp_buf_head)  free(tmp_buf_head);
                    return false;
                }
                total_blks -= total_chunk;
            }
       }

        /* 3. Collect block size buffer */
        if(state == PREPARE_BLOCK_SIZE_BUFFER)
        {
            if(blk_remain > size) {
                copy_size = size;
            } else {
                copy_size = blk_remain;
            }
            memcpy(tmp_buf,data,copy_size);
            data += copy_size;
            tmp_buf += copy_size;
            blk_remain -= copy_size;
            size -= copy_size;
            if(blk_remain == 0)
            {
               tmp_buf = tmp_buf_head; /* reset buffer head */
               //printf("Collect buffer done write it to MTD\n");
               //printf("copy size = %d\n", copy_size);
               blk_remain = blk_size; /* reset blk_remain size  */
               state = READ_BLOCK_SIZE_DATA_DONE;
            }
            else if(size == 0){
               break;
            }
       }

        /* 4. Write block size buffer to MTD */
        if(state == READ_BLOCK_SIZE_DATA_DONE)
        {
           written = write(*fd, tmp_buf_head, blk_size);
           //printf("4. Write block size buffer to MTD fd = %d written = %d blk_size = %d\n", *fd,written,blk_size);
           if(written != blk_size)
           {
              //printf("write error written is %d\n", written);
              if(tmp_buf_head)  free(tmp_buf_head);
              return false;
           }
           total_blks--;
           total_chunk--;

           if(total_blks >0)  {
               //printf("Continue prepare block size\n");
               if(total_chunk == 0)
               {
                  //printf("Read next chunk header\n");
                  state = HANDLE_CHUNK_HEADER;
               }
               else if(total_chunk > 0)
               {
                   if(copy_size != input_size)
                   {
                      data_remain = input_size-copy_size;
                      //printf("The new remain size is %d\n", data_remain);
                      if(data_remain < 0)
                      {
                          printf("Error %d", data_remain);
                      }
                      else
                      {
                          memcpy(tmp_buf,data,data_remain);
                          tmp_buf += data_remain;
                          blk_remain -= data_remain;
                      }
                   }

                   state = PREPARE_BLOCK_SIZE_BUFFER;
                   break;
               }
           }
           else
           {
               if(tmp_buf_head)  free(tmp_buf_head);
               //printf("Wrtie finished total_blks = %d\n",total_blks);
               state = WRITE_DONE;
               break;
           }
       }
   }
   return true;
}

static int mt_write_sparse_image(State* state, const char* sparse_image, const char* partition) {
    ZipArchive* za = ((UpdaterInfo*)(state->cookie))->package_zip;
    pthread_attr_t attr;
    int32_t mtd_num;
    char dev_name[64];
    const MtdPartition *mtd_partition;
    const char* partition_name;
    int ret = -1;
    int fd = -1;

    memset(dev_name, 0, sizeof(dev_name));

    const ZipEntry* sparse_entry = mzFindZipEntry(za, sparse_image);
    if(sparse_entry == NULL)
    {
      fprintf(stderr, "no file \"%s\" in package", sparse_image);
    }
        partition_name = strdup("system");

    partition_name = strdup(partition);

    printf("partition_name = %s\n", partition_name);
    mtd_scan_partitions();
    mtd_partition = mtd_find_partition_by_name(partition_name);
    if (mtd_partition == NULL) {
         printf("failed to find \"%s\" partition at /dev/mtd\n", partition_name);
         return MT_FN_FAIL_EXIT;
    }
    //Get mtd_dev_name
    mtd_num = mtd_part_to_number(mtd_partition);
    sprintf(dev_name, "/dev/mtd/mtd%d", mtd_num);
    printf("dev_name = %s\n", dev_name);


    //Erase
    printf("Erasing %s ...\n", dev_name);
    MtdWriteContext *write_context = mtd_write_partition(mtd_partition);
    if (write_context == NULL) {
        printf("format_volume: can't open MTD \"%s\"\n", dev_name);
        return MT_FN_FAIL_EXIT;
    } else if (mtd_erase_blocks(write_context, -1) == (off64_t) -1) {
        printf("format_volume: can't erase MTD \"%s\"\n", dev_name);
        mtd_write_close(write_context);
        return MT_FN_FAIL_EXIT;
    } else if (mtd_write_close(write_context)) {
        printf("format_volume: can't close MTD \"%s\"\n", dev_name);
        return MT_FN_FAIL_EXIT;
    }

    fd = TEMP_FAILURE_RETRY(open(dev_name, O_RDWR));
    ret = mzProcessZipEntryContents(za, sparse_entry, receive_new_sparse_data, &fd);
    close(fd);
    printf("Error = %d\n", ret);
    return ret;
}

Value* mtWriteSparseImageMtdFn(const char* name, State* state, int argc, Expr* argv[]) {
    char* sparse_image;   // The name of sparse image
    char* partition;      // partition
    int ret = -1;

    if (argc != 2)
        return ErrorAbort(state, "%s() expects 2 arg, got %d", name, argc);
    if (ReadArgs(state, argv, 2, &sparse_image, &partition) < 0) return NULL;

    printf("mtWriteSparseImageMtdFn Write sparse %s to %s\n", sparse_image, partition);

    ret = mt_write_sparse_image(state, sparse_image, partition);
    free(sparse_image);
    free(partition);
    return StringValue(ret == 1 ? strdup("t") : strdup(""));
}
#endif /* MTK_SLC_BUFFER_SUPPORT */

void mt_RegisterInstallFunctions(void)
{
    mt_init_partition_type();

    RegisterFunction("get_mtupdate_stage", mtGetUpdateStageFn);
    RegisterFunction("set_mtupdate_stage", mtSetUpdateStageFn);
    RegisterFunction("show_mtupdate_stage", mtShowUpdateStageFn);
    RegisterFunction("switch_active", mtSwitchActiveFn);

#ifdef MTK_SLC_BUFFER_SUPPORT
    RegisterFunction("write_sparse_image_mtd", mtWriteSparseImageMtdFn);
#endif
}

void mt_SetEmmcPreloaderWritable(State *state, const char *filename)
{
    char *buf = "0\n";
    int fd = open(filename, O_WRONLY | O_TRUNC);
    if (fd >= 0) {
        write(fd, buf, strlen(buf));
        close(fd);
    } else {
        uiPrintf(state, "can not open %s to disable force_ro\n", filename);
    }
}

int mt_checkEmmcBootHeader(State *state)
{
    int ret = 1;
    char *buf_boot1 = (char *)malloc(PRELOADER_OFFSET_EMMC);
    int len = 0;
    if (buf_boot1 == NULL) {
        uiPrintf(state, "malloc buf_boot1 %d bytes fail!\n", PRELOADER_OFFSET_EMMC);
        return 1;
    }

    char *buf_boot2 = (char *)malloc(PRELOADER_OFFSET_EMMC);
    if  (buf_boot2 == NULL)  {
        uiPrintf(state, "malloc buf_boot2 %d bytes fail!\n", PRELOADER_OFFSET_EMMC);
        free(buf_boot1);
        return 1;
    }

    memset(buf_boot1, 0, PRELOADER_OFFSET_EMMC);
    memset(buf_boot2, 0, PRELOADER_OFFSET_EMMC);
    int fd = open(PRELOADER_PART, O_RDONLY);
    if (fd < 0) {
        uiPrintf(state, "open %s fail!\n", PRELOADER_PART);
        goto done;
    }
    if(TEMP_FAILURE_RETRY(lseek(fd, 0, SEEK_SET)) == -1) {
        uiPrintf(state, "lseek %s fail!\n", PRELOADER_PART);
        goto done;
    }

    len = read(fd, buf_boot1, PRELOADER_OFFSET_EMMC);
    if (len != PRELOADER_OFFSET_EMMC)
        printf("read %s %d but only return %d data!\n", PRELOADER_PART, PRELOADER_OFFSET_EMMC, len);
    close(fd);
    fd = -1;
    fd = open(PRELOADER2_PART, O_RDWR | O_SYNC);
    if (fd < 0) {
        uiPrintf(state, "open %s fail!\n", PRELOADER2_PART);
        goto done;
    }

    if(TEMP_FAILURE_RETRY(lseek(fd, 0, SEEK_SET)) == -1) {
        uiPrintf(state, "lseek %s fail!\n", PRELOADER2_PART);
        goto done;
    }

    len = 0;
    len = read(fd, buf_boot2, PRELOADER_OFFSET_EMMC);
    if (len != PRELOADER_OFFSET_EMMC)
        printf("read %s %d but only return %d data!\n", PRELOADER_PART, PRELOADER_OFFSET_EMMC, len);
    if (memcmp(buf_boot1, buf_boot2, PRELOADER_OFFSET_EMMC)) {
        printf("header of boot1 and boot2 are different, copy header from boot1 to boot2\n");
        if(TEMP_FAILURE_RETRY(lseek(fd, 0, SEEK_SET)) == -1)  {
          uiPrintf(state, "lseek %s fail!\n", PRELOADER2_PART);
          goto done;
        }
        write(fd, buf_boot1, PRELOADER_OFFSET_EMMC);
    }
    ret = 0;
done:
    if(fd>=0) close(fd);
    if(buf_boot1) free(buf_boot1);
    if(buf_boot2) free(buf_boot2);
    return ret;
}

//  Write ext4/ubifs/yaffs raw image - log, preloader, uboot, dsp_bl, bootimage, boot, tee1, recovery
int mt_WriteRawImageFn(State* state, char **partition, char **result, Value **partition_value, Value **contents)
{
    int isPreloader = 0;
    char *dev_path = get_partition_path(*partition);
    if (!strcmp(*partition, "preloader") || !strcmp(*partition, PRELOADER_PART)) {
        isPreloader = 1;
        if (mt_get_phone_type() == EMMC_TYPE)
            mt_SetEmmcPreloaderWritable(state, PRELOADER_FORCE_RO);
    } else if (!strcmp(*partition, "preloader2") || !strcmp(*partition, PRELOADER2_PART)) {
        isPreloader = 1;
        if (mt_get_phone_type() == EMMC_TYPE) {
            mt_SetEmmcPreloaderWritable(state, PRELOADER2_FORCE_RO);
            mt_checkEmmcBootHeader(state);
        }
    }

    if (mt_get_phone_type() == NAND_TYPE) { // for nand platform, change partition name from [mtd|yaffs2]@mtd_name to mtd_name and return for google native mtd flow
#ifdef MTK_SLC_BUFFER_SUPPORT
       printf("*partition = %s \n", *partition);
       if(strstr(*partition, "preloader"))  {
          /* Get block dev name and block size */
          size_t total_size = 0, erase_size = 0, write_page_size = 0;
          char *tmp_buf = NULL;
          char *partition_name = strdup("preloader");
          char dev_name[64];
          int write_size = 0, remain_size = 0, preloader_offset = 0, written = 0;
          int mtd_num = 0;
          MtdWriteContext *erase_one_block = NULL;

          mtd_scan_partitions();
          const MtdPartition* mtd = mtd_find_partition_by_name(partition_name);
          if (mtd == NULL) {
              printf("failed to find \"%s\" partition at /dev/mtd\n", partition_name);
              goto update_fail;
          }

          //Get mtd_dev_name
          mtd_num = mtd_part_to_number(mtd);
          sprintf(dev_name, "/dev/mtd/mtd%d", mtd_num);
          printf("dev_name = %s\n", dev_name);

          mtd_partition_info(mtd,&total_size,&erase_size,&write_page_size);
          printf("erase_size = %d, write_page_size = %d \n", erase_size, write_page_size);
          if(erase_size == 0 || write_page_size == 0) {
             fprintf(stderr, "open %x fail, %s",dev_name, strerror(errno));
             goto update_fail;
          }

          // Skip preloader header and erase one block
          erase_one_block = mtd_write_partition(mtd);
          if (erase_one_block == NULL) {
              printf("Error: can't open MTD \"%s\"\n", dev_name);
              goto update_fail;
          } else if (mtd_erase(erase_one_block, erase_size) < 0) {
              printf("Error: can't erase MTD \"%s\"\n", dev_name);
              mtd_write_close(erase_one_block);
              goto update_fail;
          }

          //Write preloader
          remain_size = (*contents)->size;
          tmp_buf = (char *)malloc(write_page_size);
          if(tmp_buf == NULL) {
              printf("Malloc write_page_size fail");
              mtd_write_close(erase_one_block);
              goto update_fail;
          }

          preloader_offset = erase_size;
          //Write preloader with page size align 0xff buffer
          while (remain_size > 0)
          {
              if(remain_size >= write_page_size)
                  write_size = write_page_size;
              else
                  write_size = remain_size;

              memset(tmp_buf,0xff,write_page_size);
              memcpy(tmp_buf, ((*contents)->data), write_size);
              //printf("memcpy done preloader_offset = %d tmp_buf = %x write_size = %d\n", preloader_offset,tmp_buf, write_size);
              (*contents)->data += write_size;
              written = mtd_write_ex(erase_one_block, tmp_buf, write_page_size,preloader_offset);

              if(written != write_page_size)  {
                printf("Write preloader fail written = %d, write_page_size = %d\n",written, write_page_size);
                mtd_write_close(erase_one_block);
                goto update_fail;
              }

              preloader_offset += write_size;
              remain_size -= write_size;
              //printf("written = %d ,preloader_offset = %d remain size = %d\n", written,preloader_offset,remain_size);
          }

          if (mtd_write_close(erase_one_block)) {
              printf("format_volume: can't close MTD \"%s\"\n", dev_name);
              return MT_FN_FAIL_EXIT;
          }

           *result = *partition;
           (*contents)->data -= (*contents)->size; /* reset to the head */
           if(tmp_buf) free(tmp_buf);
           return MT_FN_SUCCESS_EXIT;

update_fail:
           if(tmp_buf) free(tmp_buf);
           *result = strdup("");
           return MT_FN_FAIL_EXIT;
        }
#endif // MTK_SLC_BUFFER_SUPPORT
        free(*partition);
        *partition = strdup(dev_path);
        (*partition_value)->data = *partition;
        free(dev_path);
        return MT_FN_SUCCESS_CONTINUE;
    }

    int fd = open(dev_path, O_WRONLY | O_SYNC);
    if (fd != -1) {
        char rbuf[512];
        int len = 0;
        int in_fd = -1;
        if (isPreloader == 1) {
            if(TEMP_FAILURE_RETRY(lseek(fd, PRELOADER_OFFSET_EMMC, SEEK_SET)) == -1) {
              fprintf(stderr, "lseek PRELOADER_OFFSET_EMMC %x fail, %s",PRELOADER_OFFSET_EMMC, strerror(errno));
              close(fd);
              free(dev_path);
              return MT_FN_FAIL_EXIT;
            }
        }
        if ((*contents)->type == VAL_STRING) {
            in_fd = open((*contents)->data, O_RDONLY);
            if (in_fd != -1) {
                while ((len = read(in_fd, rbuf, 512)) > 0) {
                    if (write(fd, rbuf, len) == -1) {
                        fprintf(stderr, "write %s to %s(%d) fail, %s\n", (*contents)->data, dev_path, len, strerror(errno));
                        *result = strdup("");
                        close(in_fd);
                        close(fd);
                        free(dev_path);
                        return MT_FN_FAIL_EXIT;
                    }
                }
                close(in_fd);
            } else {
                fprintf(stderr, "open %s fail\n", (*contents)->data);
                *result = strdup("");
                close(fd);
                free(dev_path);
                return MT_FN_FAIL_EXIT;
            }
            close(fd);
            sync();
            *result = *partition;
        } else {
            if (write(fd, (*contents)->data, (*contents)->size) != (*contents)->size) {
                close(fd);
                fprintf(stderr, "write %s fail, (%s), (*contents)->size = %d\n", dev_path, strerror(errno), (*contents)->size);
                *result = strdup("");
                free(dev_path);
                return MT_FN_FAIL_EXIT;
            } else {
                close(fd);
                sync();
                *result = *partition;
            }
        }
    } else {
        fprintf(stderr, "open %s fail\n", dev_path);
        *result = strdup("");
        free(dev_path);
        return MT_FN_FAIL_EXIT;
    }
    free(dev_path);
    return MT_FN_SUCCESS_EXIT;
}

//  Extract package file to ubifs volume
int mt_PackageExtractFileFn_ubifs(const char *name, const char *dest_path,
    ZipArchive* za, const ZipEntry* entry, bool *success)
{
#if defined(UBIFS_SUPPORT)
    if (!strcmp(dest_path, "system.img")) {
        const MtdPartition *partition;
        const char* partition_name;
        char dev_name[20];
        int32_t mtd_num;
        partition_name = strdup("system");

        mtd_scan_partitions();
        partition = mtd_find_partition_by_name(partition_name);
        if (partition == NULL) {
            printf("failed to find \"%s\" partition at /dev/mtd\n", partition_name);
            return MT_FN_FAIL_EXIT;
        }
        //Get mtd_dev_name
        mtd_num = mtd_part_to_number(partition);
        sprintf(dev_name, "/dev/mtd/mtd%d", mtd_num);
        printf("dev_name = %s\n", dev_name);

        //Erase
        MtdWriteContext *write = mtd_write_partition(partition);
        if (write == NULL) {
            printf("format_volume: can't open MTD \"%s\"\n", dev_name);
            return MT_FN_FAIL_EXIT;
        } else if (mtd_erase_blocks(write, -1) == (off64_t) -1) {
            printf("format_volume: can't erase MTD \"%s\"\n", dev_name);
            mtd_write_close(write);
            return MT_FN_FAIL_EXIT;
        } else if (mtd_write_close(write)) {
            printf("format_volume: can't close MTD \"%s\"\n", dev_name);
            return MT_FN_FAIL_EXIT;
        }

        //Extract and Write
        FILE* f = fopen(dev_name, "wb");
        if (f == NULL) {
            fprintf(stderr, "%s: can't open %s for write: %s\n", name, dev_name, strerror(errno));
            return MT_FN_FAIL_EXIT;
        }
        *success = mzExtractZipEntryToFile(za, entry, fileno(f));
        fclose(f);
        return MT_FN_SUCCESS_EXIT;
    }
#endif
    return MT_FN_SUCCESS_CONTINUE;
}

Value* mt_RebootNowFn(const char* name, State* state, char** filename)
{
    char *dev_path = get_partition_path(*filename);
    free(*filename);
    *filename = dev_path;

    int fd = open(*filename, O_RDWR | O_SYNC);
    struct bootloader_message bm;
    if (fd < 0)  {
        return ErrorAbort(state, "%s() open %s fail", name, *filename);
    }
    int count = read(fd, &bm, sizeof(bm));
    if (count != sizeof(bm)) {
        close(fd);
        return ErrorAbort(state, "%s() read %s fail, count=%d %s", name, *filename, count, strerror(errno));
    }
    memset(bm.command, 0, sizeof(bm.command));
    lseek(fd, 0, SEEK_SET);
    count = write(fd, &bm, sizeof(bm));
    if (count != sizeof(bm)) {
        close(fd);
        return ErrorAbort(state, "%s() write fail, count=%d", name, count);
    }
    if (close(fd) != 0) {
        return ErrorAbort(state, "%s() close %s fail", name, *filename);
    }
    sync();

    return NULL;
}

Value* mt_SetStageFn(const char* name, State* state, char** filename, char** stagestr)
{
    char *dev_path = get_partition_path(*filename);
    free(*filename);
    *filename = dev_path;

    //misc write needs aligment
    int fd = open(*filename, O_RDWR | O_SYNC);
    struct bootloader_message bm;
    if (fd < 0)  {
        return ErrorAbort(state, "%s() open %s fail", name, *filename);
    }
    int count = read(fd, &bm, sizeof(bm));
    if (count != sizeof(bm)) {
        close(fd);
        return ErrorAbort(state, "%s() read %s fail, count=%d %s", name, *filename, count, strerror(errno));
    }
    memset(bm.stage, 0, sizeof(bm.stage));
    snprintf(bm.stage, sizeof(bm.stage) - 1, "%s", *stagestr);

    lseek(fd, 0, SEEK_SET);
    count = write(fd, &bm, sizeof(bm));
    if (count != sizeof(bm)) {
        close(fd);
        return ErrorAbort(state, "%s() write %s fail, count=%d %s", name, *filename, count, strerror(errno));
    }
    if (close(fd) != 0) {
        return ErrorAbort(state, "%s() close %s fail", name, *filename);
    }
    sync();

    return NULL;
}

Value* mt_GetStageFn(const char* name, State* state, char** filename, char *buffer)
{
    char *dev_path = get_partition_path(*filename);
    free(*filename);
    *filename = dev_path;

    int fd = open(*filename, O_RDONLY);
    struct bootloader_message bm;

    if (fd < 0)
        return ErrorAbort(state, "%s() open %s fail", name, *filename);

    int count = read(fd, &bm, sizeof(bm));
    if (count != sizeof(bm)) {
        close(fd);
        return ErrorAbort(state, "%s() read fail, count=%d", name, count);
    }
    if (close(fd) != 0) {
        return ErrorAbort(state, "%s() close %s fail", name, *filename);
    }

    memcpy(buffer, bm.stage, sizeof(bm.stage));

    return NULL;
}

