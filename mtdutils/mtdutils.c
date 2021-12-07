/*
* Copyright (C) 2014 MediaTek Inc.
* Modification based on code covered by the mentioned copyright
* and/or permission notice(s).
*/
/*
 * Copyright (C) 2007 The Android Open Source Project
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mount.h>  // for _IOW, _IOR, mount()
#include <sys/stat.h>
#include <mtd/mtd-user.h>
#undef NDEBUG
#include <assert.h>

#include "mtdutils.h"

struct MtdPartition {
    int device_index;
    unsigned int size;
    unsigned int erase_size;
    char *name;
};

struct MtdReadContext {
    const MtdPartition *partition;
    char *buffer;
    size_t consumed;
    int fd;
};

struct MtdWriteContext {
    const MtdPartition *partition;
    char *buffer;
    size_t stored;
    int fd;

    off64_t* bad_block_offsets;
    int bad_block_alloc;
    int bad_block_count;
};

typedef struct {
    MtdPartition *partitions;
    int partitions_allocd;
    int partition_count;
} MtdState;

static MtdState g_mtd_state = {
    NULL,   // partitions
    0,      // partitions_allocd
    -1      // partition_count
};

#define MTD_PROC_FILENAME   "/proc/mtd"

int
mtd_scan_partitions()
{
    char buf[2048];
    const char *bufp;
    int fd;
    int i;
    ssize_t nbytes;

    if (g_mtd_state.partitions == NULL) {
        const int nump = 32;
        MtdPartition *partitions = malloc(nump * sizeof(*partitions));
        if (partitions == NULL) {
            errno = ENOMEM;
            return -1;
        }
        g_mtd_state.partitions = partitions;
        g_mtd_state.partitions_allocd = nump;
        memset(partitions, 0, nump * sizeof(*partitions));
    }
    g_mtd_state.partition_count = 0;

    /* Initialize all of the entries to make things easier later.
     * (Lets us handle sparsely-numbered partitions, which
     * may not even be possible.)
     */
    for (i = 0; i < g_mtd_state.partitions_allocd; i++) {
        MtdPartition *p = &g_mtd_state.partitions[i];
        if (p->name != NULL) {
            free(p->name);
            p->name = NULL;
        }
        p->device_index = -1;
    }

    /* Open and read the file contents.
     */
    fd = open(MTD_PROC_FILENAME, O_RDONLY);
    if (fd < 0) {
        goto bail;
    }
    nbytes = TEMP_FAILURE_RETRY(read(fd, buf, sizeof(buf) - 1));
    close(fd);
    if (nbytes < 0) {
        goto bail;
    }
    buf[nbytes] = '\0';

    /* Parse the contents of the file, which looks like:
     *
     *     # cat /proc/mtd
     *     dev:    size   erasesize  name
     *     mtd0: 00080000 00020000 "bootloader"
     *     mtd1: 00400000 00020000 "mfg_and_gsm"
     *     mtd2: 00400000 00020000 "0000000c"
     *     mtd3: 00200000 00020000 "0000000d"
     *     mtd4: 04000000 00020000 "system"
     *     mtd5: 03280000 00020000 "userdata"
     */
    bufp = buf;
    while (nbytes > 0) {
        int mtdnum, mtdsize, mtderasesize;
        int matches;
        char mtdname[64];
        mtdname[0] = '\0';
        mtdnum = -1;

        matches = sscanf(bufp, "mtd%d: %x %x \"%63[^\"]",
                &mtdnum, &mtdsize, &mtderasesize, mtdname);
        /* This will fail on the first line, which just contains
         * column headers.
         */
        if (matches == 4) {
            MtdPartition *p = &g_mtd_state.partitions[mtdnum];
            p->device_index = mtdnum;
            p->size = mtdsize;
            p->erase_size = mtderasesize;
            p->name = strdup(mtdname);
            if (p->name == NULL) {
                errno = ENOMEM;
                goto bail;
            }
            g_mtd_state.partition_count++;
        }

        /* Eat the line.
         */
        while (nbytes > 0 && *bufp != '\n') {
            bufp++;
            nbytes--;
        }
        if (nbytes > 0) {
            bufp++;
            nbytes--;
        }
    }

    return g_mtd_state.partition_count;

bail:
    // keep "partitions" around so we can free the names on a rescan.
    g_mtd_state.partition_count = -1;
    return -1;
}

const MtdPartition *
mtd_find_partition_by_name(const char *name)
{
    if (g_mtd_state.partitions != NULL) {
        int i;
        for (i = 0; i < g_mtd_state.partitions_allocd; i++) {
            MtdPartition *p = &g_mtd_state.partitions[i];
            if (p->device_index >= 0 && p->name != NULL) {
                if (strcmp(p->name, name) == 0) {
                    return p;
                }
            }
        }
    }
    return NULL;
}

int
mtd_mount_partition(const MtdPartition *partition, const char *mount_point,
        const char *filesystem, int read_only)
{
    const unsigned long flags = MS_NOATIME | MS_NODEV | MS_NODIRATIME;
    char devname[64];
    int rv = -1;

    sprintf(devname, "/dev/block/mtdblock%d", partition->device_index);
    if (!read_only) {
        rv = mount(devname, mount_point, filesystem, flags, NULL);
    }
    if (read_only || rv < 0) {
        rv = mount(devname, mount_point, filesystem, flags | MS_RDONLY, 0);
        if (rv < 0) {
            printf("Failed to mount %s on %s: %s\n",
                    devname, mount_point, strerror(errno));
        } else {
            printf("Mount %s on %s read-only\n", devname, mount_point);
        }
    }
#if 1   //TODO: figure out why this is happening; remove include of stat.h
    if (rv >= 0) {
        /* For some reason, the x bits sometimes aren't set on the root
         * of mounted volumes.
         */
        struct stat st;
        rv = stat(mount_point, &st);
        if (rv < 0) {
            return rv;
        }
        mode_t new_mode = st.st_mode | S_IXUSR | S_IXGRP | S_IXOTH;
        if (new_mode != st.st_mode) {
printf("Fixing execute permissions for %s\n", mount_point);
            rv = chmod(mount_point, new_mode);
            if (rv < 0) {
                printf("Couldn't fix permissions for %s: %s\n",
                        mount_point, strerror(errno));
            }
        }
    }
#endif
    return rv;
}

int
mtd_partition_info(const MtdPartition *partition,
        size_t *total_size, size_t *erase_size, size_t *write_size)
{
    char mtddevname[32];
    sprintf(mtddevname, "/dev/mtd/mtd%d", partition->device_index);
    int fd = open(mtddevname, O_RDONLY);
    if (fd < 0) return -1;

    struct mtd_info_user mtd_info;
    int ret = ioctl(fd, MEMGETINFO, &mtd_info);
    close(fd);
    if (ret < 0) return -1;

    if (total_size != NULL) *total_size = mtd_info.size;
    if (erase_size != NULL) *erase_size = mtd_info.erasesize;
    if (write_size != NULL) *write_size = mtd_info.writesize;
    return 0;
}

MtdReadContext *mtd_read_partition(const MtdPartition *partition)
{
    MtdReadContext *ctx = (MtdReadContext*) malloc(sizeof(MtdReadContext));
    if (ctx == NULL) return NULL;

    ctx->buffer = malloc(partition->erase_size);
    if (ctx->buffer == NULL) {
        free(ctx);
        return NULL;
    }

    char mtddevname[32];
    sprintf(mtddevname, "/dev/mtd/mtd%d", partition->device_index);
    ctx->fd = open(mtddevname, O_RDONLY);
    if (ctx->fd < 0) {
        free(ctx->buffer);
        free(ctx);
        return NULL;
    }

    ctx->partition = partition;
    ctx->consumed = partition->erase_size;
    return ctx;
}

static int read_block(const MtdPartition *partition, int fd, char *data)
{
    struct mtd_ecc_stats before, after;
    if (ioctl(fd, ECCGETSTATS, &before)) {
        fprintf(stderr, "mtd: ECCGETSTATS error (%s)\n", strerror(errno));
        return -1;
    }

    off64_t pos = TEMP_FAILURE_RETRY(lseek64(fd, 0, SEEK_CUR));
    if (pos == -1) {
        printf("mtd: read_block: couldn't SEEK_CUR: %s\n", strerror(errno));
        return -1;
    }

    ssize_t size = partition->erase_size;
    int mgbb;

    while (pos + size <= (int) partition->size) {
        if (TEMP_FAILURE_RETRY(lseek64(fd, pos, SEEK_SET)) != pos ||
                    TEMP_FAILURE_RETRY(read(fd, data, size)) != size) {
            fprintf(stderr, "mtd: read error at 0x%08llx (%s)\n",
                    (long long)pos, strerror(errno));
        } else if (ioctl(fd, ECCGETSTATS, &after)) {
            fprintf(stderr, "mtd: ECCGETSTATS error (%s)\n", strerror(errno));
            return -1;
        } else if (after.failed != before.failed) {
            fprintf(stderr, "mtd: ECC errors (%d soft, %d hard) at 0x%08llx\n",
                    after.corrected - before.corrected,
                    after.failed - before.failed, (long long)pos);
            // copy the comparison baseline for the next read.
            memcpy(&before, &after, sizeof(struct mtd_ecc_stats));
        } else if ((mgbb = ioctl(fd, MEMGETBADBLOCK, &pos))) {
            fprintf(stderr,
                    "mtd: MEMGETBADBLOCK returned %d at 0x%08llx: %s\n",
                    mgbb, (long long)pos, strerror(errno));
        } else {
            return 0;  // Success!
        }

        pos += partition->erase_size;
    }

    errno = ENOSPC;
    return -1;
}

ssize_t mtd_read_data(MtdReadContext *ctx, char *data, size_t len)
{
    size_t read = 0;
    while (read < len) {
        if (ctx->consumed < ctx->partition->erase_size) {
            size_t avail = ctx->partition->erase_size - ctx->consumed;
            size_t copy = len - read < avail ? len - read : avail;
            memcpy(data + read, ctx->buffer + ctx->consumed, copy);
            ctx->consumed += copy;
            read += copy;
        }

        // Read complete blocks directly into the user's buffer
        while (ctx->consumed == ctx->partition->erase_size &&
               len - read >= ctx->partition->erase_size) {
            if (read_block(ctx->partition, ctx->fd, data + read)) return -1;
            read += ctx->partition->erase_size;
        }

        if (read >= len) {
            return read;
        }

        // Read the next block into the buffer
        if (ctx->consumed == ctx->partition->erase_size && read < len) {
            if (read_block(ctx->partition, ctx->fd, ctx->buffer)) return -1;
            ctx->consumed = 0;
        }
    }

    return read;
}

ssize_t mtd_read_data_ex(MtdReadContext *ctx, char *data, size_t size, off64_t offset)
{
    off64_t pos = lseek64(ctx->fd, offset, SEEK_SET);
    if (pos != offset) {
        fprintf(stderr, "can not move file pointer 0x%llX 0x%llX\n", pos, offset);
    }

    int r = read(ctx->fd, data, size);
    if (r != (int) size) {
        fprintf(stderr, "can not random read %d %zd\n", r, size);
    }

    return r;
}

void mtd_read_close(MtdReadContext *ctx)
{
    close(ctx->fd);
    free(ctx->buffer);
    free(ctx);
}

int mtd_part_to_number(const MtdPartition *partition)
{
	if(partition){
		return partition->device_index;
	}
	return -1;
}

MtdWriteContext *mtd_write_partition(const MtdPartition *partition)
{
    MtdWriteContext *ctx = (MtdWriteContext*) malloc(sizeof(MtdWriteContext));
    if (ctx == NULL) return NULL;

    ctx->bad_block_offsets = NULL;
    ctx->bad_block_alloc = 0;
    ctx->bad_block_count = 0;

    ctx->buffer = malloc(partition->erase_size);
    if (ctx->buffer == NULL) {
        free(ctx);
        return NULL;
    }

    char mtddevname[32];
    sprintf(mtddevname, "/dev/mtd/mtd%d", partition->device_index);
    ctx->fd = open(mtddevname, O_RDWR);
    if (ctx->fd < 0) {
        free(ctx->buffer);
        free(ctx);
        return NULL;
    }

    ctx->partition = partition;
    ctx->stored = 0;
    return ctx;
}

static void add_bad_block_offset(MtdWriteContext *ctx, off64_t pos) {
    if (ctx->bad_block_count + 1 > ctx->bad_block_alloc) {
        ctx->bad_block_alloc = (ctx->bad_block_alloc*2) + 1;
        ctx->bad_block_offsets = realloc(ctx->bad_block_offsets,
                                         ctx->bad_block_alloc * sizeof(off64_t));
    }
    ctx->bad_block_offsets[ctx->bad_block_count++] = pos;
}

static int write_block(MtdWriteContext *ctx, const char *data)
{
    const MtdPartition *partition = ctx->partition;
    int fd = ctx->fd;

    off64_t pos = TEMP_FAILURE_RETRY(lseek64(fd, 0, SEEK_CUR));
    if (pos == (off64_t) -1) {
        printf("mtd: write_block: couldn't SEEK_CUR: %s\n", strerror(errno));
        return -1;
    }

    ssize_t size = partition->erase_size;
    while (pos + size <= (int) partition->size) {
        off64_t bpos = pos;
        int ret = ioctl(fd, MEMGETBADBLOCK, &bpos);
        if (ret != 0 && !(ret == -1 && errno == EOPNOTSUPP)) {
            add_bad_block_offset(ctx, pos);
            fprintf(stderr,
                    "mtd: not writing bad block at 0x%08llx (ret %d): %s\n",
                    pos, ret, strerror(errno));
            pos += partition->erase_size;
            continue;  // Don't try to erase known factory-bad blocks.
        }

        struct erase_info_user erase_info;
        erase_info.start = pos;
        erase_info.length = size;
        int retry;
        for (retry = 0; retry < 2; ++retry) {
            if (ioctl(fd, MEMERASE, &erase_info) < 0) {
                fprintf(stderr, "mtd: erase failure at 0x%08llx (%s)\n",
                        pos, strerror(errno));
                continue;
            }
            if (TEMP_FAILURE_RETRY(lseek64(fd, pos, SEEK_SET)) != pos ||
                TEMP_FAILURE_RETRY(write(fd, data, size)) != size) {
                fprintf(stderr, "mtd: write error at 0x%08llx (%s)\n",
                        pos, strerror(errno));
            }

            char verify[size];
            if (TEMP_FAILURE_RETRY(lseek64(fd, pos, SEEK_SET)) != pos ||
                TEMP_FAILURE_RETRY(read(fd, verify, size)) != size) {
                fprintf(stderr, "mtd: re-read error at 0x%08llx (%s)\n",
                        pos, strerror(errno));
                continue;
            }
            if (memcmp(data, verify, size) != 0) {
                fprintf(stderr, "mtd: verification error at 0x%08llx (%s)\n",
                        pos, strerror(errno));
                continue;
            }

            if (retry > 0) {
                fprintf(stderr, "mtd: wrote block after %d retries\n", retry);
            }
            fprintf(stderr, "mtd: successfully wrote block at %llx\n", pos);
            return 0;  // Success!
        }

        // Try to erase it once more as we give up on this block
        add_bad_block_offset(ctx, pos);
        fprintf(stderr, "mtd: skipping write block at 0x%08llx\n", pos);
        ioctl(fd, MEMERASE, &erase_info);
        pos += partition->erase_size;
    }

    // Ran out of space on the device
    errno = ENOSPC;
    return -1;
}

ssize_t mtd_write_data(MtdWriteContext *ctx, const char *data, size_t len)
{
    size_t wrote = 0;
    while (wrote < len) {
        // Coalesce partial writes into complete blocks
        if (ctx->stored > 0 || len - wrote < ctx->partition->erase_size) {
            size_t avail = ctx->partition->erase_size - ctx->stored;
            size_t copy = len - wrote < avail ? len - wrote : avail;
            memcpy(ctx->buffer + ctx->stored, data + wrote, copy);
            ctx->stored += copy;
            wrote += copy;
        }

        // If a complete block was accumulated, write it
        if (ctx->stored == ctx->partition->erase_size) {
            if (write_block(ctx, ctx->buffer)) return -1;
            ctx->stored = 0;
        }

        // Write complete blocks directly from the user's buffer
        while (ctx->stored == 0 && len - wrote >= ctx->partition->erase_size) {
            if (write_block(ctx, data + wrote)) return -1;
            wrote += ctx->partition->erase_size;
        }
    }

    return wrote;
}

int mtd_write_block_ex(MtdWriteContext *ctx, const char *data, off64_t addr)
{
    const MtdPartition *partition = ctx->partition;
    int fd = ctx->fd;

    off64_t pos = lseek64(fd, addr, SEEK_SET);
    if (pos == (off64_t) -1) return 1;

    fprintf(stdout, "[%s] addr=0x%X, pos=0x%X, erase=0x%X\n", __func__, (int) addr, (int) pos, partition->erase_size);

    ssize_t size = partition->erase_size;
    while (pos + size <= (int) partition->size) {
        off64_t bpos = pos;
        if (ioctl(fd, MEMGETBADBLOCK, &bpos) > 0) {
            add_bad_block_offset(ctx, pos);
            fprintf(stderr, "mtd: not writing bad block at 0x%08llx\n", pos);
            pos += partition->erase_size;
            continue;  // Don't try to erase known factory-bad blocks.
        }

        struct erase_info_user erase_info;
        erase_info.start = pos;
        erase_info.length = size;
        int retry;
        for (retry = 0; retry < 2; ++retry) {
            fprintf(stdout, "[%s] erase\n", __func__);
            if (ioctl(fd, MEMERASE, &erase_info) < 0) {
                fprintf(stderr, "mtd: erase failure at 0x%08llx (%s)\n",
                        pos, strerror(errno));
                continue;
            }
            fprintf(stdout, "[%s] write\n", __func__);
            if (lseek64(fd, pos, SEEK_SET) != pos ||
                write(fd, data, size) != size) {
                fprintf(stderr, "mtd: write error at 0x%08llx (%s)\n",
                        pos, strerror(errno));
            }

            char verify[size];
            fprintf(stdout, "[%s] read back\n", __func__);
            if (lseek64(fd, pos, SEEK_SET) != pos ||
                read(fd, verify, size) != size) {
                fprintf(stderr, "mtd: re-read error at 0x%08llx (%s)\n",
                        pos, strerror(errno));
                continue;
            }
            fprintf(stdout, "[%s] compare : %X %X %X %X %X %X %X %X\n", __func__,
                    verify[0], verify[1], verify[2], verify[3],
                    verify[4], verify[5], verify[6], verify[7]);
            if (memcmp(data, verify, size) != 0) {
                fprintf(stderr, "mtd: verification error at 0x%08llx (%s)\n",
                        pos, strerror(errno));
                continue;
            }

            if (retry > 0) {
                fprintf(stderr, "mtd: wrote block after %d retries\n", retry);
            }
            return 0;  // Success!
        }

        fprintf(stdout, "[%s] write next\n", __func__);

        // Try to erase it once more as we give up on this block
        add_bad_block_offset(ctx, pos);
        fprintf(stderr, "mtd: skipping write block at 0x%08llx\n", pos);
        ioctl(fd, MEMERASE, &erase_info);
        pos += partition->erase_size;
    }

    // Ran out of space on the device
    errno = ENOSPC;
    return -1;
}

int mtd_write_data_ex(MtdWriteContext *ctx, const char *data, size_t size, off64_t offset)
{
    off64_t pos = lseek64(ctx->fd, offset, SEEK_SET);
    if (pos != offset)  {
        fprintf(stderr, "can not move file pointer %llX %llX\n", pos, offset);
    }

    //int len = write(ctx->fd, data, size);
    int len = mtd_write_data(ctx, data, size);
    if (len != (int) size)  {
        fprintf(stderr, "can not random read %d %zd\n", len, size);
    }

    return len;
}

off64_t mtd_erase_blocks(MtdWriteContext *ctx, int blocks)
{
    // Zero-pad and write any pending data to get us to a block boundary
    if (ctx->stored > 0) {
        size_t zero = ctx->partition->erase_size - ctx->stored;
        memset(ctx->buffer + ctx->stored, 0, zero);
        if (write_block(ctx, ctx->buffer)) return -1;
        ctx->stored = 0;
    }

    off64_t pos = TEMP_FAILURE_RETRY(lseek64(ctx->fd, 0, SEEK_CUR));
    if ((off_t) pos == (off_t) -1) {
        printf("mtd_erase_blocks: couldn't SEEK_CUR: %s\n", strerror(errno));
        return -1;
    }

    const int total = (ctx->partition->size - pos) / ctx->partition->erase_size;
    if (blocks < 0) blocks = total;
    if (blocks > total) {
        errno = ENOSPC;
        return -1;
    }

    // Erase the specified number of blocks
    while (blocks-- > 0) {
        off64_t bpos = pos;
        if (ioctl(ctx->fd, MEMGETBADBLOCK, &bpos) > 0) {
            fprintf(stderr, "mtd: not erasing bad block at 0x%08llx\n", pos);
            pos += ctx->partition->erase_size;
            continue;  // Don't try to erase known factory-bad blocks.
        }

        struct erase_info_user erase_info;
        erase_info.start = pos;
        erase_info.length = ctx->partition->erase_size;
        if (ioctl(ctx->fd, MEMERASE, &erase_info) < 0) {
            fprintf(stderr, "mtd: erase failure at 0x%08llx\n", pos);
        }
        pos += ctx->partition->erase_size;
    }

    return pos;
}

int mtd_write_close(MtdWriteContext *ctx)
{
    int r = 0;
    // Make sure any pending data gets written
    if (mtd_erase_blocks(ctx, 0) == (off64_t) -1) r = -1;
    if (close(ctx->fd)) r = -1;
    free(ctx->bad_block_offsets);
    free(ctx->buffer);
    free(ctx);
    return r;
}

int mtd_write_ex(MtdWriteContext *ctx, const char *data, size_t size, off64_t offset)
{
    off64_t pos = lseek64(ctx->fd, offset, SEEK_SET);
    if (pos != offset)  {
        fprintf(stderr, "can not move file pointer %llX %llX\n", pos, offset);
    }

    int len = write(ctx->fd, data, size);
    if (len != (int) size)  {
        fprintf(stderr, "can not random read %d %zd\n", len, size);
    }

    return len;
}

off64_t mtd_erase(MtdWriteContext *ctx, off64_t pos)
{
        struct erase_info_user erase_info;
        erase_info.start = (int)pos;
        erase_info.length = ctx->partition->erase_size;
        //printf("mtd_erase start = %d, pos = %d length = %d\n",(int)erase_info.start,(int)pos, (int)erase_info.length);
        if (ioctl(ctx->fd, MEMERASE, &erase_info) < 0) {
            fprintf(stderr, "mtd: erase failure at 0x%08llx\n", pos);
            return -1;
        }
        return pos;
}
