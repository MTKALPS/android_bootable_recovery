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
 *
 * $DateTime$
 * $Revision$
 * $Author$
 */
#ifndef BACKUP_RESTORE_H
#define BACKUP_RESTORE_H

#include "minzip/Zip.h"
#include "minzip/SysUtil.h"
extern "C" {
int userdata_backup(void);
int userdata_restore(char *filename, int mode);
int check_part_size(ZipArchive *zip, int path_is_data, const char *zip_path, MemMapping *pMap,bool needs_mount);
int check_ota(const char *filename);
extern char backup_path[PATH_MAX];
extern int part_size_changed;
extern int usrdata_changed;
enum {
    CHECK_OK = 0,
    ERROR_INVALID_ARGS,
    ERROR_OTA_FILE,
    ERROR_FILE_OPEN,
    ERROR_FILE_WRITE,
    ERROR_OUT_OF_MEMORY,
    ERROR_PARTITION_SETTING,
    ERROR_ONLY_FULL_CHANGE_SIZE,
    ERROR_ACCESS_SD,
    ERROR_ACCESS_USERDATA,
    ERROR_SD_FREE_SPACE,
    ERROR_SD_WRITE_PROTECTED,
    ERROR_USERDATA_FREE_SPACE,
    ERROR_MATCH_DEVICE,
    ERROR_DIFFERENTIAL_VERSION,
    ERROR_BUILD_PROP,
    NEED_TO_CHECK_FREE_SD,
};

int special_factory_reset(void);
}
#endif //BACKUP_RESTORE_H

