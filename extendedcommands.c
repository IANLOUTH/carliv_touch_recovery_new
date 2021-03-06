/*
 * Copyright (C) 2014 The CyanogenMod Project
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
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sys/wait.h>
#include <sys/limits.h>
#include <dirent.h>
#include <sys/stat.h>

#include <sys/statfs.h>
#include <sys/vfs.h>

#include <signal.h>
#include <sys/wait.h>

#include "bootloader.h"
#include "libcrecovery/common.h"
#include "common.h"
#include "cutils/android_reboot.h"
#include "cutils/properties.h"
#include "install.h"
#include "make_ext4fs.h"
#include "minuictr/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "recovery_ui.h"

#include "extendedcommands.h"
#include "nandroid.h"
#include "mtdutils/mounts.h"
#include "flashutils/flashutils.h"
#include "edify/expr.h"
#include <libgen.h>
#include "mtdutils/mtdutils.h"
#include "bmlutils/bmlutils.h"
#include "mmcutils/mmcutils.h"

#include "adb_install.h"
#include "minadbd/adb.h"
#include "fuse_sideload.h"
#include "fuse_sdcard_provider.h"

extern struct selabel_handle *sehandle;

// Prototypes of private functions that are used before defined
static void show_choose_zip_menu(const char *mount_point);
static int can_partition(const char* volume);
static int is_path_mounted(const char* path);

int get_filtered_menu_selection(const char** headers, char** items, int menu_only, int initial_selection, int items_count) {
    int index;
    int offset = 0;
    int* translate_table = (int*)malloc(sizeof(int) * items_count);
    for (index = 0; index < items_count; index++) {
        if (items[index] == NULL)
            continue;
        char *item = items[index];
        items[index] = NULL;
        items[offset] = item;
        translate_table[offset] = index;
        offset++;
    }
    items[offset] = NULL;

    initial_selection = translate_table[initial_selection];
    int ret = get_menu_selection(headers, items, menu_only, initial_selection);
    if (ret < 0 || ret >= offset) {
        free(translate_table);
        return ret;
    }

    ret = translate_table[ret];
    free(translate_table);
    return ret;
}

static void write_string_to_file(const char* filename, const char* string) {
    ensure_path_mounted(filename);
    char tmp[PATH_MAX];
    sprintf(tmp, "mkdir -p $(dirname %s)", filename);
    __system(tmp);
    FILE *file = fopen(filename, "w");
    if (file != NULL) {
        fprintf(file, "%s", string);
        fclose(file);
    }
}

void write_recovery_version() {
    char path[PATH_MAX];
    sprintf(path, "%s/%s", get_primary_storage_path(), RECOVERY_VERSION_FILE);
    write_string_to_file(path, EXPAND(RECOVERY_VERSION) "-" EXPAND(TARGET_DEVICE));
    // force unmount /data for /data/media devices as we call this on recovery exit
    preserve_data_media(0);
    ensure_path_unmounted(path);
    preserve_data_media(1);   
}

static void write_last_install_path(const char* install_path) {
    char path[PATH_MAX];
    sprintf(path, "%s/%s", get_primary_storage_path(), RECOVERY_LAST_INSTALL_FILE);
    write_string_to_file(path, install_path);
}

static char* read_last_install_path() {
    static char path[PATH_MAX];
    sprintf(path, "%s/%s", get_primary_storage_path(), RECOVERY_LAST_INSTALL_FILE);

    ensure_path_mounted(path);
    FILE *f = fopen(path, "r");
    if (f != NULL) {
        fgets(path, PATH_MAX, f);
        fclose(f);

        return path;
    }
    return NULL;
}

static void toggle_signature_check() {
    signature_check_enabled = !signature_check_enabled;
    ui_print("Signature Check: %s\n", signature_check_enabled ? "Enabled" : "Disabled");
}

#ifdef ENABLE_LOKI
int loki_support_enabled = 1;
void toggle_loki_support() {
    loki_support_enabled = !loki_support_enabled;
    ui_print("Loki Support: %s\n", loki_support_enabled ? "Enabled" : "Disabled");
}
#endif

void toggle_md5_check() {
    md5_check_enabled = !md5_check_enabled;
    ui_print("md5 Check: %s\n", md5_check_enabled ? "Enabled" : "Disabled");
}

void toggle_vibration() {
    vibration_enabled = !vibration_enabled;
    ui_print("Vibrate on touch: %s\n", vibration_enabled ? "Enabled" : "Disabled");
}

#define POWER_ITEM_RECOVERY	    0
#define POWER_ITEM_BOOTLOADER   1
#define POWER_ITEM_POWEROFF	    2

void show_power_menu() {
	
	const char* headers[] = { "Power Options", NULL };
    
    char* power_items[4];
    
	power_items[0] = "Reboot Recovery";
	char bootloader_mode[PROPERTY_VALUE_MAX];
	property_get("ro.bootloader.mode", bootloader_mode, "");
	if (!strcmp(bootloader_mode, "download")) {
	power_items[1] = "Reboot to Download";
	} else {
	power_items[1] = "Reboot to Bootloader";
	}
	power_items[2] = "Power Off";
	power_items[3] = NULL;
	
	for (;;) {
		int chosen_item = get_menu_selection(headers, power_items, 0, 0);
		if (chosen_item == GO_BACK || chosen_item == REFRESH)
			break;
		switch (chosen_item) {
		  case POWER_ITEM_RECOVERY:
		  {
			ui_print("Rebooting recovery...\n");
			reboot_main_system(ANDROID_RB_RESTART2, 0, "recovery");
			break;
		   }
		  case POWER_ITEM_BOOTLOADER:
		  {
			if (!strcmp(bootloader_mode, "download")) {
			  ui_print("Rebooting to download mode...\n");
#ifdef BOARD_HAS_MTK_CPU
              reboot_main_system(ANDROID_RB_POWEROFF, 0, 0);
              break;
#else                    
			  reboot_main_system(ANDROID_RB_RESTART2, 0, "download");
			  break;
#endif
			} else {
			  ui_print("Rebooting to bootloader...\n");
			  reboot_main_system(ANDROID_RB_RESTART2, 0, "bootloader");
			  break;
			}
		  }
		  case POWER_ITEM_POWEROFF:
		  {
			ui_print("Shutting down...\n");
			reboot_main_system(ANDROID_RB_POWEROFF, 0, 0);
			break;
		  }
	   }
	}
}

void show_install_update_menu() {
    char* primary_path = get_primary_storage_path();
    char* extra_path = get_extra_storage_path();
    char* usb_path = get_usb_storage_path();
    int num_extra_volumes = get_num_extra_volumes();
    int wipe_cache = 0;
    
    const char* headers[] = { "Install from zip file", NULL };
    
    char* install_menu_items[] = {  "Choose zip from Sdcard",
                                    "Install zip from sideload",
                                    "Choose from last install folder",
                                    "Multi-zip Installer",
                                    "Toggle Signature Verification",
                                    NULL,
                                    NULL,
                                    NULL 
    };

	if (num_extra_volumes != 0) {
		if (extra_path != NULL) 
			install_menu_items[5] = "Choose zip from ExtraSD";	
    }
    if (usb_path != NULL && ensure_path_mounted(usb_path) == 0) {
        install_menu_items[6] = "Choose zip from USB-Drive";
	}
    
    for (;;) {
        int chosen_item = get_filtered_menu_selection(headers, install_menu_items, 0, 0, sizeof(install_menu_items) / sizeof(char*));
        if (chosen_item == GO_BACK || chosen_item == REFRESH)
			break;
        switch (chosen_item)
        {
            case 0:
                show_choose_zip_menu(primary_path);
                break;
            case 1:
	            enter_sideload_mode(&wipe_cache);
                break;                
            case 2:
	            {
					char *last_path_used = read_last_install_path();
		            if (last_path_used == NULL)
		                show_choose_zip_menu(primary_path);
		            else
		                show_choose_zip_menu(last_path_used);
	                break;
	            }                
            case 3:
                show_multi_flash_menu();
                break;
            case 4:
                toggle_signature_check();
                break;
            case 5:
                show_choose_zip_menu(extra_path);
                break;
            case 6:
                show_choose_zip_menu(usb_path);
                break;
            default:
                return;
        }

    }
}

void wipe_battery_stats(int confirm) {
	if (confirm && !confirm_selection( "Confirm reset battery stats?", "Yes - Reset battery stats"))
        return;
	if (!is_encrypted_data()) ensure_path_mounted("/data");
    device_wipe_battery_stats();
    ui_print("\n-- Resetting battery stats...\n");
    __system("rm -f /data/system/batterystats.bin");
	ui_print("Battery stats resetted.\n");
    if (!is_encrypted_data()) ensure_path_unmounted("/data");
}

void show_wipe_menu() {

    const char* headers[] = { "Wipe Menu", NULL };
    
	char* wipe_items[] = { "Wipe Data - Factory Reset",
						   "Wipe Cache",
						   "Wipe Dalvik Cache",
						   "Wipe ALL - Preflash",
						   NULL };

	for (;;) {
		int chosen_item = get_filtered_menu_selection(headers, wipe_items, 0, 0, sizeof(wipe_items) / sizeof(char*));
		if (chosen_item == GO_BACK || chosen_item == REFRESH)
			break;
		switch (chosen_item) {
		  case 0:
			wipe_data(ui_text_visible());
			if (!ui_text_visible()) return;
			break;
		  case 1:
			wipe_cache(ui_text_visible());
			if (!ui_text_visible()) return;
			break;
		  case 2:
			wipe_dalvik_cache(ui_text_visible());
			if (!ui_text_visible()) return;
			break;
		  case 3:
			wipe_preflash(ui_text_visible());
			if (!ui_text_visible()) return;
			break;
		}
	}    
}

static void free_string_array(char** array) {
    if (array == NULL)
        return;
    char* cursor = array[0];
    int i = 0;
    while (cursor != NULL) {
        free(cursor);
        cursor = array[++i];
    }
    free(array);
}

static int strcmpi(const char *str1, const char *str2) {
    int i = 0;
    int ret = 0;

    while (ret == 0 && str1[i] && str2[i]) {
        ret = tolower(str1[i]) - tolower(str2[i]);
        ++i;
    }

    return ret;
}

static char** gather_files(const char* basedir, const char* fileExtensionOrDirectory, int* numFiles) {
    DIR *dir;
    struct dirent *de;
    int total = 0;
    int i;
    char** files = NULL;
    int pass;
    *numFiles = 0;
    int dirLen = strlen(basedir);
    char directory[PATH_MAX];

    // Append a trailing slash if necessary
    strcpy(directory, basedir);
    if (directory[dirLen - 1] != '/') {
        strcat(directory, "/");
        ++dirLen;
    }

    dir = opendir(directory);
    if (dir == NULL) {
        ui_print("Couldn't open directory %s\n", directory);
        return NULL;
    }

    unsigned int extension_length = 0;
    if (fileExtensionOrDirectory != NULL)
        extension_length = strlen(fileExtensionOrDirectory);

    i = 0;
    for (pass = 0; pass < 2; pass++) {
        while ((de = readdir(dir)) != NULL) {
            // skip hidden files
            if (de->d_name[0] == '.')
                continue;

            if (fileExtensionOrDirectory != NULL) {
                if (strcmp("", fileExtensionOrDirectory) == 0) {
                    struct stat info;
                    char fullFileName[PATH_MAX];
                    strcpy(fullFileName, directory);
                    strcat(fullFileName, de->d_name);
                    lstat(fullFileName, &info);
                    // make sure it is not a directory
                    if (S_ISDIR(info.st_mode))
                        continue;
                } else {
                    // make sure that we can have the desired extension (prevent seg fault)
                    if (strlen(de->d_name) < extension_length)
                        continue;
                    // compare the extension
                    if (strcmp(de->d_name + strlen(de->d_name) - extension_length, fileExtensionOrDirectory) != 0)
                        continue;
                }
            } else {
                struct stat info;
                char fullFileName[PATH_MAX];
                strcpy(fullFileName, directory);
                strcat(fullFileName, de->d_name);
                lstat(fullFileName, &info);
                // make sure it is a directory
                if (!(S_ISDIR(info.st_mode)))
                    continue;
            }

            if (pass == 0) {
                total++;
                continue;
            }

            files[i] = (char*)malloc(dirLen + strlen(de->d_name) + 2);
            strcpy(files[i], directory);
            strcat(files[i], de->d_name);
            if (fileExtensionOrDirectory == NULL)
                strcat(files[i], "/");
            i++;
        }
        if (pass == 1)
            break;
        if (total == 0)
            break;
            
        rewinddir(dir);
        *numFiles = total;
        files = (char**)malloc((total + 1) * sizeof(char*));
        files[total] = NULL;
    }

    if (closedir(dir) < 0) {
        LOGE("Failed to close directory.\n");
    }

    if (total == 0) {
        return NULL;
    }

    // sort the result
    if (files != NULL) {
        for (i = 0; i < total; i++) {
            int curMax = -1;
            int j;
            for (j = 0; j < total - i; j++) {
                if (curMax == -1 || strcmpi(files[curMax], files[j]) < 0)
                    curMax = j;
            }
            char* temp = files[curMax];
            files[curMax] = files[total - i - 1];
            files[total - i - 1] = temp;
        }
    }

    return files;
}

// pass in NULL for fileExtensionOrDirectory and you will get a directory chooser
int no_files_found = 0;
static char* choose_file_menu(const char* basedir, const char* fileExtensionOrDirectory, const char* headers[]) {
    const char* fixed_headers[20];
    int numFiles = 0;
    int numDirs = 0;
    int i;
    char* return_value = NULL;
    char directory[PATH_MAX];
    int dir_len = strlen(basedir);

    strcpy(directory, basedir);

    // Append a trailing slash if necessary
    if (directory[dir_len - 1] != '/') {
        strcat(directory, "/");
        dir_len++;
    }

    i = 0;
    while (headers[i]) {
        fixed_headers[i] = headers[i];
        i++;
    }
    fixed_headers[i] = directory;
    fixed_headers[i + 1] = NULL;

    char** files = gather_files(directory, fileExtensionOrDirectory, &numFiles);
    char** dirs = NULL;
    if (fileExtensionOrDirectory != NULL)
        dirs = gather_files(directory, NULL, &numDirs);
    int total = numDirs + numFiles;
    if (total == 0) {
        no_files_found = 1; // we found no valid file to select
        ui_print("No files found.\n");
    } else {
		no_files_found = 0; // we found a valid file to select
        char** list = (char**)malloc((total + 1) * sizeof(char*));
        list[total] = NULL;


        for (i = 0; i < numDirs; i++) {
            list[i] = strdup(dirs[i] + dir_len);
        }

        for (i = 0; i < numFiles; i++) {
            list[numDirs + i] = strdup(files[i] + dir_len);
        }

        for (;;) {
            int chosen_item = get_menu_selection(fixed_headers, list, 0, 0);
            if (chosen_item == GO_BACK || chosen_item == REFRESH)
                break;
            if (chosen_item < numDirs) {
                char* subret = choose_file_menu(dirs[chosen_item], fileExtensionOrDirectory, headers);
                if (subret != NULL) {
                    return_value = strdup(subret);
                    free(subret);
                    break;
                }
                continue;
            }
            return_value = strdup(files[chosen_item - numDirs]);
            break;
        }
        free_string_array(list);
    }

    free_string_array(files);
    free_string_array(dirs);
    return return_value;
}

static void show_choose_zip_menu(const char *mount_point) {
    if (ensure_path_mounted(mount_point) != 0) {
        LOGE("Can't mount %s\n", mount_point);
        return;
    }

    const char* headers[] = { "Choose a zip to apply", NULL };

    char* file = choose_file_menu(mount_point, ".zip", headers);
    if (file == NULL)
        return;
    char confirm[PATH_MAX];
    sprintf(confirm, "Yes - Install %s", basename(file));

    if (confirm_selection("Confirm install?", confirm)) {
        install_zip(file);
        write_last_install_path(dirname(file));
    }

    free(file);
}

/*****************************************/
/*         Multi-Flash Zip code          */
/*      Original code by PhilZ @xda      */
/*         adapted by carliv @ xda       */
/*****************************************/
#define MULTI_ZIP_FOLDER "clockworkmod/multi_zip"

void show_multi_flash_menu() {
	char* primary_path = get_primary_storage_path();
    char* extra_path = get_extra_storage_path();
    char* usb_path = get_usb_storage_path();
    int num_extra_volumes = get_num_extra_volumes();
    
    const char* headers_dir[] = { "Choose a set of zip files",
                                   NULL
    };
    const char* headers[] = {  "Select files to install...",
                                NULL
    };
    
    char tmp[PATH_MAX];
    char dirz[PATH_MAX];
    char* zip_folder = NULL;

    //look for MULTI_ZIP_FOLDER in /sdcard
    struct stat st;
    ensure_path_mounted(primary_path);
    sprintf(dirz, "%s/%s", primary_path, MULTI_ZIP_FOLDER);
    sprintf(tmp, "mkdir -p %s ; chmod 777 %s", dirz, dirz);
    __system(tmp);
    sprintf(tmp, "%s/", dirz);
    stat(tmp, &st);
    if (S_ISDIR(st.st_mode)) {
        zip_folder = choose_file_menu(tmp, NULL, headers_dir);
        // zip_folder = NULL if no subfolders found or user chose Go Back
        if (no_files_found) {
            ui_print("No subfolders with zip files in %s\n", dirz);
            ui_print("Looking in the second sdcard...\n");
        }
    } else
        LOGI("%s not found. Searching the second sdcard...\n", tmp);

    // case MULTI_ZIP_FOLDER not found, or no subfolders or user selected Go Back:
    // search for MULTI_ZIP_FOLDER in other_sd
    struct stat s;
    if (num_extra_volumes != 0) {
	    if (extra_path != NULL) {
	        ensure_path_mounted(extra_path);
	        sprintf(dirz, "%s/%s", extra_path, MULTI_ZIP_FOLDER);
		    sprintf(tmp, "mkdir -p %s ; chmod 777 %s", dirz, dirz);
		    __system(tmp);
		    sprintf(tmp, "%s/", dirz);
	        stat(tmp, &s);
	        if (zip_folder == NULL && S_ISDIR(s.st_mode)) {
	            zip_folder = choose_file_menu(tmp, NULL, headers_dir);
	            if (no_files_found)
	                ui_print("No zip files found in %s\n", dirz);
	        }
	    }
	}
	
	if (usb_path != NULL && ensure_path_mounted(usb_path) == 0) {
        sprintf(dirz, "%s/%s", usb_path, MULTI_ZIP_FOLDER);
	    sprintf(tmp, "mkdir -p %s ; chmod 777 %s", dirz, dirz);
	    __system(tmp);
	    sprintf(tmp, "%s/", dirz);
        stat(tmp, &s);
        if (zip_folder == NULL && S_ISDIR(s.st_mode)) {
            zip_folder = choose_file_menu(tmp, NULL, headers_dir);
            if (no_files_found)
                ui_print("No zip files found in %s\n", dirz);
        }
    }

    // either MULTI_ZIP_FOLDER path not found (ui_print help)
    // or it was found but no subfolder (ui_print help above in no_files_found)
    // or user chose Go Back every time: return silently
    if (zip_folder == NULL) {
        if (!(S_ISDIR(st.st_mode)) && !(S_ISDIR(s.st_mode)))
            ui_print("Create at least 1 folder with your zip files under %s\n", dirz);
        return;
    }

    //gather zip files list
    int dir_len = strlen(zip_folder);
    int numFiles = 0;
    char** files = gather_files(zip_folder, ".zip", &numFiles);
    if (numFiles == 0) {
        ui_print("No zip files found under %s\n", zip_folder);
    } else {
        // start showing multi-zip menu
        char** list = (char**) malloc((numFiles + 3) * sizeof(char*));
        list[0] = strdup("Select/Unselect All");
        list[1] = strdup(">> Flash Selected Files <<");
        list[numFiles+2] = NULL; // Go Back Menu

        int i;
        for(i=2; i < numFiles+2; i++) {
            list[i] = strdup(files[i-2] + dir_len - 4);
            strncpy(list[i], "(x) ", 4);
        }

        int select_all = 1;
        int chosen_item;
        for (;;)
        {
            chosen_item = get_menu_selection(headers, list, 0, 0);
            if (chosen_item == GO_BACK || chosen_item == REFRESH)
                break;
            if (chosen_item == 1)
                break;
            if (chosen_item == 0) {
                // select / unselect all
                select_all ^= 1;
                for(i=2; i < numFiles+2; i++) {
                    if (select_all) strncpy(list[i], "(x)", 3);
                    else strncpy(list[i], "( )", 3);
                }
            } else if (strncmp(list[chosen_item], "( )", 3) == 0) {
                strncpy(list[chosen_item], "(x)", 3);
            } else if (strncmp(list[chosen_item], "(x)", 3) == 0) {
                strncpy(list[chosen_item], "( )", 3);
            }
        }

        //flashing selected zip files
        if (chosen_item == 1) {
            static char confirm[PATH_MAX];
            sprintf(confirm, "Yes - Install from %s", basename(zip_folder));
            if (confirm_selection("Install selected files?", confirm))
            {
                for(i=2; i < numFiles+2; i++) {
                    if (strncmp(list[i], "(x)", 3) == 0) {
                        if (install_zip(files[i-2]) != 0)
                            break;
                    }
                }
            }
        }
        free_string_array(list);
    }
    free_string_array(files);
}
//-------- End Multi-Flash Zip code

void show_nandroid_restore_menu(const char* path) {
    if (ensure_path_mounted(path) != 0) {
        LOGE("Can't mount %s\n", path);
        return;
    }

    const char* headers[] = { "Choose an image to restore", NULL };

    char tmp[PATH_MAX];
    sprintf(tmp, "%s/clockworkmod/backup/", path);
    char* file = choose_file_menu(tmp, NULL, headers);
    if (file == NULL)
        return;

    if (confirm_selection("Confirm restore?", "Yes - Restore"))
        nandroid_restore(file, 1, 1, 1, 1, 1, 0);

    free(file);
}

static void show_nandroid_delete_menu(const char* path) {
    if (ensure_path_mounted(path) != 0) {
        LOGE("Can't mount %s\n", path);
        return;
    }

    const char* headers[] = { "Choose a backup to delete", NULL };

    char tmp[PATH_MAX];
    sprintf(tmp, "%s/clockworkmod/backup/", path);
    char* file = choose_file_menu(tmp, NULL, headers);
    if (file == NULL)
        return;

    if (confirm_selection("Confirm delete?", "Yes - Delete")) {
        // nandroid_restore(file, 1, 1, 1, 1, 1, 0);
        ui_print("-- Deleting %s\n", basename(file));
        sprintf(tmp, "rm -rf %s", file);
        __system(tmp);
        ui_print("Backup deleted!\n");
    }

    free(file);
}

#define MAX_NUM_USB_VOLUMES 3
#define LUN_FILE_EXPANDS    2

struct lun_node {
    const char *lun_file;
    struct lun_node *next;
};

static struct lun_node *lun_head = NULL;
static struct lun_node *lun_tail = NULL;

int control_usb_storage_set_lun(Volume* vol, bool enable, const char *lun_file) {
    const char *vol_device = enable ? vol->device : "";
    int fd;
    struct lun_node *node;

    // Verify that we have not already used this LUN file
    for(node = lun_head; node; node = node->next) {
        if (!strcmp(node->lun_file, lun_file)) {
            // Skip any LUN files that are already in use
            return -1;
        }
    }

    // Open a handle to the LUN file
    LOGI("Trying %s on LUN file %s\n", vol->device, lun_file);
    if ((fd = open(lun_file, O_WRONLY)) < 0) {
        LOGW("Unable to open ums lunfile %s (%s)\n", lun_file, strerror(errno));
        return -1;
    }

    // Write the volume path to the LUN file
    if ((write(fd, vol_device, strlen(vol_device) + 1) < 0) &&
       (!enable || !vol->device2 || (write(fd, vol->device2, strlen(vol->device2)) < 0))) {
        LOGW("Unable to write to ums lunfile %s (%s)\n", lun_file, strerror(errno));
        close(fd);
        return -1;
    } else {
        // Volume path to LUN association succeeded
        close(fd);

        // Save off a record of this lun_file being in use now
        node = (struct lun_node *)malloc(sizeof(struct lun_node));
        node->lun_file = strdup(lun_file);
        node->next = NULL;
        if (lun_head == NULL)
           lun_head = lun_tail = node;
        else {
           lun_tail->next = node;
           lun_tail = node;
        }

        LOGI("Successfully %sshared %s on LUN file %s\n", enable ? "" : "un", vol->device, lun_file);
        return 0;
    }
}

int control_usb_storage_for_lun(Volume* vol, bool enable) {
    static const char* lun_files[] = {
#ifdef BOARD_UMS_LUNFILE
        BOARD_UMS_LUNFILE,
#endif
#ifdef TARGET_USE_CUSTOM_LUN_FILE_PATH
        TARGET_USE_CUSTOM_LUN_FILE_PATH,
#endif
        "/sys/class/android_usb/android0/f_mass_storage/lun0/file",
        "/sys/class/android_usb/android0/f_mass_storage/lun/file",
        "/sys/class/android_usb/android%d/f_mass_storage/lun/file",
        "/sys/class/android_usb/android0/f_mass_storage/lun_ex/file",
        "/sys/devices/platform/usb_mass_storage/lun%d/file",
        "/sys/devices/platform/msm_hsusb/gadget/lun0/file",
        "/sys/devices/virtual/android_usb/android0/f_mass_storage/lun",
        NULL
    };

    // If recovery.fstab specifies a LUN file, use it
    if (vol->lun) {
        return control_usb_storage_set_lun(vol, enable, vol->lun);
    }

    // Try to find a LUN for this volume
    //   - iterate through the lun file paths
    //   - expand any %d by LUN_FILE_EXPANDS
    int lun_num = 0;
    int i;
    for(i = 0; lun_files[i]; i++) {
        const char *lun_file = lun_files[i];
        for(lun_num = 0; lun_num < LUN_FILE_EXPANDS; lun_num++) {
            char formatted_lun_file[255];
    
            // Replace %d with the LUN number
            bzero(formatted_lun_file, 255);
            snprintf(formatted_lun_file, 254, lun_file, lun_num);
    
            // Attempt to use the LUN file
            if (control_usb_storage_set_lun(vol, enable, formatted_lun_file) == 0) {
                return 0;
            }
        }
    }

    // All LUNs were exhausted and none worked
    LOGW("Could not %sable %s on LUN %d\n", enable ? "en" : "dis", vol->device, lun_num);

    return -1;  // -1 failure, 0 success
}

int control_usb_storage(Volume **volumes, bool enable) {
    int res = -1;
    int i;
    for(i = 0; i < MAX_NUM_USB_VOLUMES; i++) {
        Volume *volume = volumes[i];
        if (volume) {
            int vol_res = control_usb_storage_for_lun(volume, enable);
            if (vol_res == 0) res = 0; // if any one path succeeds, we return success
        }
    }

    // Release memory used by the LUN file linked list
    struct lun_node *node = lun_head;
    while(node) {
       struct lun_node *next = node->next;
       free((void *)node->lun_file);
       free(node);
       node = next;
    }
    lun_head = lun_tail = NULL;

    return res;  // -1 failure, 0 success
}

void show_mount_usb_storage_menu() {
	char* primary_path = get_primary_storage_path();
    char* extra_path = get_extra_storage_path();
    // Build a list of Volume objects; some or all may not be valid
    Volume* volumes[MAX_NUM_USB_VOLUMES] = {
        volume_for_path(primary_path),
        volume_for_path(extra_path)
    };

    // Enable USB storage
    if (control_usb_storage(volumes, 1))
        return;

    const char* headers[] = { "USB Mass Storage device",
							 "Leaving this menu unmounts",
							 "your SD card from your PC.",
							 NULL
    };

    static char* list[] = { "Unmount", NULL };

    for (;;) {
        int chosen_item = get_menu_selection(headers, list, 0, 0);
        if (chosen_item == GO_BACK || chosen_item == REFRESH || chosen_item == 0)
            break;
    }

    // Disable USB storage
    control_usb_storage(volumes, 0);
}

int confirm_selection(const char* title, const char* confirm) {
    struct stat info;
    int ret = 0;

    char path[PATH_MAX];
    sprintf(path, "%s/%s", get_primary_storage_path(), RECOVERY_NO_CONFIRM_FILE);
    ensure_path_mounted(path);
    if (0 == stat(path, &info))
        return 1;

    int many_confirm;
    char* confirm_str = strdup(confirm);
    const char* confirm_headers[] = { title, "  THIS CAN NOT BE UNDONE.", NULL };
    int old_val = ui_is_showing_back_button();
    ui_set_showing_back_button(0);

    sprintf(path, "%s/%s", get_primary_storage_path(), RECOVERY_MANY_CONFIRM_FILE);
    ensure_path_mounted(path);
    many_confirm = 0 == stat(path, &info);

    if (many_confirm) {
        char* items[] = { "No",
                          "No",
                          "No",
                          "No",
                          "No",
                          "No",
                          "No",
                          confirm_str, // Yes, [7]
                          "No",
                          "No",
                          "No",
                          NULL };
        int chosen_item = get_menu_selection(confirm_headers, items, 0, 0);
        ret = (chosen_item == 7);
    } else {
        char* items[] = { "No",
                          confirm_str, // Yes, [1]
                          NULL };
        int chosen_item = get_menu_selection(confirm_headers, items, 0, 0);
        ret = (chosen_item == 1);
    }

    free(confirm_str);
    ui_set_showing_back_button(old_val);
    return ret;
}

static int exec_cmd(const char* path, char* const argv[]) {
    int status;
    pid_t child;
    if ((child = vfork()) == 0) {
        execv(path, argv);
        _exit(-1);
    }
    waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        //printf("%s failed with status %d\n", path, WEXITSTATUS(status));
    }
    return WEXITSTATUS(status);
}

typedef struct {
    char mount[256];
    char unmount[256];
    Volume* v;
} MountMenuEntry;

typedef struct {
    char txt[256];
    Volume* v;
} FormatMenuEntry;

static int is_safe_to_format(const char* name) {
    char str[512];    
    char* partition;
    property_get("ro.ctr.forbid_format", str, "/misc,/radio,/bootloader,/recovery,/efs,/wimax");

	if (is_encrypted_data()) strncat(str, ",/data", 6);

    partition = strtok(str, ", ");
    while (partition != NULL) {
        if (strcmp(name, partition) == 0) {
            return 0;
        }
        partition = strtok(NULL, ", ");
    }

    return 1;
}

static int is_allowed_to_mount(const char* name) {
    char str[512];
    char* partition;
    property_get("ro.ctr.forbid_mount", str, "/misc,/radio,/bootloader,/recovery,/efs,/wimax");

    partition = strtok(str, ", ");
    while (partition != NULL) {
        if (strcmp(name, partition) == 0) {
            return 0;
        }
        partition = strtok(NULL, ", ");
    }

    return 1;
}

void show_partition_menu() {
    const char* headers[] = { "Mounts and Storage Menu", NULL };

    static MountMenuEntry* mount_menu = NULL;
    static FormatMenuEntry* format_menu = NULL;
    
    typedef char* string;
    
    int i, mountable_volumes, formatable_volumes;
    int num_volumes;
    Volume* device_volumes;

    num_volumes = get_num_volumes();
    device_volumes = get_device_volumes();

    string options[255];

    if(!device_volumes)
        return;

    mountable_volumes = 0;
    formatable_volumes = 0;

    mount_menu = malloc(num_volumes * sizeof(MountMenuEntry));
    format_menu = malloc(num_volumes * sizeof(FormatMenuEntry));

    for (i = 0; i < num_volumes; ++i) {
        Volume* v = &device_volumes[i];
        if(strcmp("ramdisk", v->fs_type) != 0 && strcmp("mtd", v->fs_type) != 0 && strcmp("emmc", v->fs_type) != 0 && strcmp("bml", v->fs_type) != 0 && is_allowed_to_mount(v->mount_point)) {
            if (strcmp("datamedia", v->fs_type) != 0) {
                sprintf(mount_menu[mountable_volumes].mount, "Mount %s", v->mount_point);
                sprintf(mount_menu[mountable_volumes].unmount, "Unmount %s", v->mount_point);
                mount_menu[mountable_volumes].v = &device_volumes[i];
                ++mountable_volumes;
            }
            if (is_safe_to_format(v->mount_point)) {
                sprintf(format_menu[formatable_volumes].txt, "Format %s", v->mount_point);
                format_menu[formatable_volumes].v = &device_volumes[i];
                ++formatable_volumes;
            }
        }
        else if (strcmp("ramdisk", v->fs_type) != 0 && strcmp("mtd", v->fs_type) == 0 && is_safe_to_format(v->mount_point))
        {
            sprintf(format_menu[formatable_volumes].txt, "Format %s", v->mount_point);
            format_menu[formatable_volumes].v = &device_volumes[i];
            ++formatable_volumes;
        }
    }

    static char* confirm_format  = "Confirm format?";
    static char* confirm = "Yes - Format";
    char confirm_string[255];
    char* primary_path = get_primary_storage_path();
    char* extra_path = get_extra_storage_path();

    for (;;)
    {
        for (i = 0; i < mountable_volumes; i++)
        {
            MountMenuEntry* e = &mount_menu[i];
            Volume* v = e->v;
            if(is_path_mounted(v->mount_point))
                options[i] = e->unmount;
            else
                options[i] = e->mount;
        }

        for (i = 0; i < formatable_volumes; i++)
        {
            FormatMenuEntry* e = &format_menu[i];

            options[mountable_volumes + i] = e->txt;
        }

        if (!is_data_media()) {
            options[mountable_volumes + formatable_volumes] = "Mount USB storage";
            options[mountable_volumes + formatable_volumes + 1] = NULL;
        } else if (is_data_media_volume_path(primary_path) && extra_path == NULL) {
            options[mountable_volumes + formatable_volumes] = "Format /data and /data/media";
            options[mountable_volumes + formatable_volumes + 1] = NULL;
        } else {
            options[mountable_volumes + formatable_volumes] = "Format /data and /data/media";
            options[mountable_volumes + formatable_volumes + 1] = "Mount USB storage";
            options[mountable_volumes + formatable_volumes + 2] = NULL;
        }

        int chosen_item = get_menu_selection(headers, options, 0, 0);
        if (chosen_item == GO_BACK || chosen_item == REFRESH)
            break;
        if (chosen_item == (mountable_volumes + formatable_volumes)) {
            if (!is_data_media()) {
                show_mount_usb_storage_menu();
            } else {
                if (!confirm_selection("Format /data and /data/media", confirm))
                    continue;
                if (is_encrypted_data()) {
	                preserve_data_media(0);
	                set_encryption_state(0);
	                ensure_path_unmounted("/data");
	                ui_print("Formatting /data...\n");
	                if (0 != format_volume("/data"))
	                    ui_print("Error formatting /data!\n");
	                else
	                    ui_print("Done.\n");
	                preserve_data_media(1);
		            ui_print("Rebooting recovery...\n");
		            sleep(1);
					reboot_main_system(ANDROID_RB_RESTART2, 0, "recovery");
				} else {
					preserve_data_media(0);
	                ensure_path_unmounted("/data");
	                ui_print("Formatting /data...\n");
	                if (0 != format_volume("/data"))
	                    ui_print("Error formatting /data!\n");
	                else
	                    ui_print("Done.\n");
	                preserve_data_media(1);
	                // recreate /data/media with proper permissions
		            ensure_path_mounted("/data");
		            setup_data_media();
				}
            }
        } else if (is_data_media() && chosen_item == (mountable_volumes+formatable_volumes+1)) {
            show_mount_usb_storage_menu();
        } else if (chosen_item < mountable_volumes) {
            MountMenuEntry* e = &mount_menu[chosen_item];
            Volume* v = e->v;

            if (is_path_mounted(v->mount_point)) {
				preserve_data_media(0);
                if (0 != ensure_path_unmounted(v->mount_point))
                    ui_print("Error unmounting %s!\n", v->mount_point);
                preserve_data_media(1);
            } else {
                if (0 != ensure_path_mounted(v->mount_point))
                    ui_print("Error mounting %s!\n",  v->mount_point);
            }
        } else if (chosen_item < (mountable_volumes + formatable_volumes)) {
            chosen_item = chosen_item - mountable_volumes;
            FormatMenuEntry* e = &format_menu[chosen_item];
            Volume* v = e->v;

            sprintf(confirm_string, "%s - %s", v->mount_point, confirm_format);

            if (strcmp(v->fs_type, "auto") == 0) {
                if (can_partition(v->mount_point)) format_sdcard(v->mount_point);
                continue;
            }
            if (!confirm_selection(confirm_string, confirm))
                continue;
            ui_print("Formatting %s...\n", v->mount_point);
            if (0 != format_volume(v->mount_point))
                ui_print("Error formatting %s!\n", v->mount_point);
            else
                ui_print("Done.\n");
        }
    }

    free(mount_menu);
    free(format_menu);
}

static int show_nandroid_advanced_backup_menu(const char* path) {
    if (ensure_path_mounted(path) != 0) {
        LOGE("Can't mount %s\n", path);
        return 0;
    }

	const char* headers[] = { "Advanced Backup", "", "Select partition(s) to backup:", NULL };
    
    int backup_list[6];
    char* backup_item[6];
    
    backup_list[0] = 0;
    backup_list[1] = 0;
    backup_list[2] = 0;
    backup_list[3] = 0;
    backup_list[4] = 0;
        
    backup_item[5] = NULL;
    
    int cont = 1;
    int ret = 0;
    for (;cont;) {
		if (backup_list[0] == 0)
			backup_item[0] = "Select boot:    ";
		else
			backup_item[0] = "Backup boot: (+)";
	    	
	    if (backup_list[1] == 0)
    		backup_item[1] = "Select system:    ";
	    else
	    	backup_item[1] = "Backup system: (+)";

	    if (backup_list[2] == 0)
	    	backup_item[2] = "Select data:    ";
	    else
	    	backup_item[2] = "Backup data: (+)";

	    if (backup_list[3] == 0)
	    	backup_item[3] = "Select cache:    ";
	    else
	    	backup_item[3] = "Backup cache: (+)";
    	if (backup_list[4] == 0)
	    	backup_item[4] = "Perform Backup";
	    	
	    int chosen_item = get_menu_selection(headers, backup_item, 0, 0);
	    if (chosen_item == GO_BACK || chosen_item == REFRESH)
            break;
	    switch (chosen_item) {
			case 0: 
			backup_list[0] = !backup_list[0];			
				continue;
			case 1:
			backup_list[1] = !backup_list[1];
				continue;
			case 2:
			backup_list[2] = !backup_list[2]; 
				continue;
			case 3: 
			backup_list[3] = !backup_list[3];
				continue;	
		   
			case 4: cont = 0;
				break;
		}
	}
	
	char backup_path[PATH_MAX];
	time_t t = time(NULL);
    struct tm *tmp = localtime(&t);
    if (tmp == NULL) {
        struct timeval tp;
        gettimeofday(&tp, NULL);
        snprintf(backup_path, PATH_MAX, "%s/clockworkmod/backup/adv-%ld", path, tp.tv_sec);
    } else {
        char str[PATH_MAX];
        strftime(str, PATH_MAX, "%F-%H-%M-%S", tmp);
        snprintf(backup_path, PATH_MAX, "%s/clockworkmod/backup/adv-%s", path, str);
    }
    
    if (!cont) {
		ret = nandroid_advanced_backup(backup_path, backup_list[0], backup_list[1], backup_list[2], backup_list[3]);	
	}
	
	return ret; 	
}

static int show_nandroid_advanced_restore_menu(const char* path) {
    if (ensure_path_mounted(path) != 0) {
        LOGE("Can't mount %s\n", path);
        return 0;
    }

	const char* advancedheaders[] = { "Choose a backup folder to restore",
									 "",
									 "Choose a folder to restore",
									 "first. The next menu will",
									 "show you more options.",
									 NULL };
    
    char tmp[PATH_MAX];
    sprintf(tmp, "%s/clockworkmod/backup/", path);
    char* file = choose_file_menu(tmp, NULL, advancedheaders);
    if (file == NULL)
        return 0;

    const char* headers[] = { "Advanced Restore", "", "Select image(s) to restore:", NULL };
    
    int restore_list[6];
    char* restore_item[6];
    
    restore_list[0] = 0;
    restore_list[1] = 0;
    restore_list[2] = 0;
    restore_list[3] = 0;
    restore_list[4] = 0;    
    
    restore_item[5] = NULL;
    
    int cont = 1;
    int ret = 0;
    for (;cont;) {
		if (restore_list[0] == 0)
			restore_item[0] = "Select boot:    ";
		else
			restore_item[0] = "Restore boot: (+)";
	    	
	    if (restore_list[1] == 0)
    		restore_item[1] = "Select system:    ";
	    else
	    	restore_item[1] = "Restore system: (+)";

	    if (restore_list[2] == 0)
	    	restore_item[2] = "Select data:    ";
	    else
	    	restore_item[2] = "Restore data: (+)";

	    if (restore_list[3] == 0)
	    	restore_item[3] = "Select cache:    ";
	    else
	    	restore_item[3] = "Restore cache: (+)";
    	if (restore_list[4] == 0)
	    	restore_item[4] = "Start Restore";
	    	
	    int chosen_item = get_menu_selection(headers, restore_item, 0, 0);
	    if (chosen_item == GO_BACK || chosen_item == REFRESH)
            break;
	    switch (chosen_item) {
			case 0: 
			restore_list[0] = !restore_list[0];			
				continue;
			case 1:
			restore_list[1] = !restore_list[1];
				continue;
			case 2:
			restore_list[2] = !restore_list[2];
				continue;
			case 3: 
			restore_list[3] = !restore_list[3];
				continue;	
		   
			case 4: cont = 0;
				break;
		}
	}
    
    if (!cont) {
		ret = nandroid_advanced_restore(file, restore_list[0], restore_list[1], restore_list[2], restore_list[3]);	
	}
	
	return ret; 	
}

#ifdef BOARD_HAS_MTK_CPU

//=========================================/
//=  MTK partitions special backup menu   =/
//=            carliv @ xda               =/
//=========================================/

static int show_mtk_advanced_backup_menu(const char* path) {
    if (ensure_path_mounted(path) != 0) {
        LOGE("Can't mount %s\n", path);
        return 0;
    }

	const char* headers[] = { "Advanced MTK Backup", "", "Select partition(s) to backup:", NULL };
    
    int backup_mtk[6];
    char* mtkb_item[6];
    
    backup_mtk[0] = 0;
    backup_mtk[1] = 0;
    backup_mtk[2] = 0;
    backup_mtk[3] = 0;
    backup_mtk[4] = 0;
        
    mtkb_item[5] = NULL;
    
    int cont = 1;
    int ret = 0;
    for (;cont;) {
		if (backup_mtk[0] == 0)
			mtkb_item[0] = "Select uboot:    ";
		else
			mtkb_item[0] = "Backup uboot: (+)";
	    	
	    if (backup_mtk[1] == 0)
    		mtkb_item[1] = "Select logo:    ";
	    else
	    	mtkb_item[1] = "Backup logo: (+)";

	    if (backup_mtk[2] == 0)
	    	mtkb_item[2] = "Select nvram:    ";
	    else
	    	mtkb_item[2] = "Backup nvram: (+)";

	    if (backup_mtk[3] == 0)
	    	mtkb_item[3] = "Select secro:    ";
	    else
	    	mtkb_item[3] = "Backup secro: (+)";
    	if (backup_mtk[4] == 0)
	    	mtkb_item[4] = "Perform Backup";
	    	
	    int chosen_item = get_menu_selection(headers, mtkb_item, 0, 0);
	    if (chosen_item == GO_BACK || chosen_item == REFRESH)
            break;
	    switch (chosen_item) {
			case 0: 
			backup_mtk[0] = !backup_mtk[0];			
				continue;
			case 1:
			backup_mtk[1] = !backup_mtk[1];
				continue;
			case 2:
			backup_mtk[2] = !backup_mtk[2]; 
				continue;
			case 3: 
			backup_mtk[3] = !backup_mtk[3];
				continue;	
		   
			case 4: cont = 0;
				break;
		}
	}
	
	char backup_path[PATH_MAX];
	time_t t = time(NULL);
    struct tm *tmp = localtime(&t);
    if (tmp == NULL) {
        struct timeval tp;
        gettimeofday(&tp, NULL);
        snprintf(backup_path, PATH_MAX, "%s/clockworkmod/mtk-backups/MTK-%ld", path, tp.tv_sec);
    } else {
        char str[PATH_MAX];
        strftime(str, PATH_MAX, "%F-%H-%M-%S", tmp);
        snprintf(backup_path, PATH_MAX, "%s/clockworkmod/mtk-backups/MTK-%s", path, str);
    }
    
    if (!cont) {
		ret = nandroid_mtk_backup(backup_path, backup_mtk[0], backup_mtk[1], backup_mtk[2], backup_mtk[3]);	
	}
	
	return ret; 	
}

static int show_mtk_advanced_restore_menu(const char* path) {
    if (ensure_path_mounted(path) != 0) {
        LOGE("Can't mount %s\n", path);
        return 0;
    }

	const char* advancedheaders[] = { "Choose a mtk backup folder to restore",
									 "",
									 "Choose a folder to restore",
									 "first. The next menu will",
									 "show you more options.",
									 NULL };
    
    char tmp[PATH_MAX];
    sprintf(tmp, "%s/clockworkmod/mtk-backups/", path);
    char* file = choose_file_menu(tmp, NULL, advancedheaders);
    if (file == NULL)
        return 0;

    const char* headers[] = { "Advanced MTK Restore", "", "Select image(s) to restore:", NULL };
    
    int restore_mtk[6];
    char* mtkr_item[6];
    
    restore_mtk[0] = 0;
    restore_mtk[1] = 0;
    restore_mtk[2] = 0;
    restore_mtk[3] = 0;
    restore_mtk[4] = 0;    
    
    mtkr_item[5] = NULL;
    
    int cont = 1;
    int ret = 0;
    for (;cont;) {
		if (restore_mtk[0] == 0)
			mtkr_item[0] = "Select uboot:    ";
		else
			mtkr_item[0] = "Restore uboot: (+)";
	    	
	    if (restore_mtk[1] == 0)
    		mtkr_item[1] = "Select logo:    ";
	    else
	    	mtkr_item[1] = "Restore logo: (+)";

	    if (restore_mtk[2] == 0)
	    	mtkr_item[2] = "Select nvram:    ";
	    else
	    	mtkr_item[2] = "Restore nvram: (+)";

	    if (restore_mtk[3] == 0)
	    	mtkr_item[3] = "Select secro:    ";
	    else
	    	mtkr_item[3] = "Restore secro: (+)";
    	if (restore_mtk[4] == 0)
	    	mtkr_item[4] = "Start Restore";
	    	
	    int chosen_item = get_menu_selection(headers, mtkr_item, 0, 0);
	    if (chosen_item == GO_BACK || chosen_item == REFRESH)
            break;
	    switch (chosen_item) {
			case 0: 
			restore_mtk[0] = !restore_mtk[0];			
				continue;
			case 1:
			restore_mtk[1] = !restore_mtk[1];
				continue;
			case 2:
			restore_mtk[2] = !restore_mtk[2];
				continue;
			case 3: 
			restore_mtk[3] = !restore_mtk[3];
				continue;	
		   
			case 4: cont = 0;
				break;
		}
	}
    
    if (!cont) {
		ret = nandroid_mtk_restore(file, restore_mtk[0], restore_mtk[1], restore_mtk[2], restore_mtk[3]);	
	}
	
	return ret; 	
}

static void show_mtk_delete_menu(const char* path) {
    if (ensure_path_mounted(path) != 0) {
        LOGE("Can't mount %s\n", path);
        return;
    }

    const char* headers[] = { "Choose a backup to delete", NULL };

    char tmp[PATH_MAX];
    sprintf(tmp, "%s/clockworkmod/mtk-backups/", path);
    char* file = choose_file_menu(tmp, NULL, headers);
    if (file == NULL)
        return;

    if (confirm_selection("Confirm delete?", "Yes - Delete")) {
        ui_print("-- Deleting %s\n", basename(file));
        sprintf(tmp, "rm -rf %s", file);
        __system(tmp);
        ui_print("Backup deleted!\n");
    }

    free(file);
}


static void show_mtk_special_backup_restore_menu() {
	char* primary_path = get_primary_storage_path();
    char* extra_path = get_extra_storage_path();
    char* usb_path = get_usb_storage_path();
    int num_extra_volumes = get_num_extra_volumes();

    const char* headers[] = { "MTK Partitions Backup/Restore", NULL };

    char* list[] = { "MTK Backup to SDcard",
                     "MTK Restore from SDcard",
                     "Delete a mtk backup from SDcard",                     
                      NULL,
                      NULL,
                      NULL,
                      NULL,
                      NULL,
                      NULL,
                      NULL
    };

    if (num_extra_volumes != 0) {
	    if (extra_path != NULL) {
	        list[3] = "MTK Backup to ExtraSD";
	        list[4] = "MTK Restore from ExtraSD";
	        list[5] = "Delete a mtk backup from ExtraSD";
	    }
	}
    if (usb_path != NULL && ensure_path_mounted(usb_path) == 0) {
		list[6] = "MTK Backup to USB-Drive";
        list[7] = "MTK Restore from USB-Drive";
        list[8] = "Delete a mtk backup from USB-Drive";
	}

    for (;;) {
        int chosen_item = get_filtered_menu_selection(headers, list, 0, 0, sizeof(list) / sizeof(char*));
        if (chosen_item == GO_BACK || chosen_item == REFRESH)
            break;
		switch (chosen_item)
        	{	
			case 0:
				show_mtk_advanced_backup_menu(primary_path);
                break;
            case 1:
                show_mtk_advanced_restore_menu(primary_path);
                break;
            case 2:
                show_mtk_delete_menu(primary_path);
                break;
            case 3:
                show_mtk_advanced_backup_menu(extra_path);
                break;
            case 4:
                show_mtk_advanced_restore_menu(extra_path);
                break;
            case 5:
                show_mtk_delete_menu(extra_path);
                break;
            case 6:
                show_mtk_advanced_backup_menu(usb_path);
                break;
            case 7:
                show_mtk_advanced_restore_menu(usb_path);
                break;
            case 8:
                show_mtk_delete_menu(usb_path);
                break;    
          }
    }
    
}
//=========================================/
#endif

static void choose_default_backup_format() {
    const char* headers[] = { "Default Backup Format", NULL };

    int fmt = nandroid_get_default_backup_format();

    char **list;
    char* list_tar_default[] = { "tar (default)",
                                 "tar + gzip",
                                 NULL };
    char* list_tgz_default[] = { "tar",
                                 "tar + gzip (default)",
                                 NULL };

    if (fmt == NANDROID_BACKUP_FORMAT_TGZ) {
        list = list_tgz_default;
    } else {
        list = list_tar_default;
    }
    
    char path[PATH_MAX];
    sprintf(path, "%s/%s", get_primary_storage_path(), NANDROID_BACKUP_FORMAT_FILE);
    int chosen_item = get_menu_selection(headers, list, 0, 0);
    switch (chosen_item) {
        case 0:
            write_string_to_file(path, "tar");
            ui_print("Default backup format set to tar.\n");
            break;
        case 1:
            write_string_to_file(path, "tgz");
            ui_print("Default backup format set to tar + gzip.\n");
            break;
    }
}

static void show_nandroid_advanced_menu() {
	char* primary_path = get_primary_storage_path();
    char* extra_path = get_extra_storage_path();
    char* usb_path = get_usb_storage_path();
    int num_extra_volumes = get_num_extra_volumes();

    const char* headers[] = { "Advanced Backup and Restore", NULL };

    char* list[] = { "Advanced Backup to SDcard",
                     "Advanced Restore from SDcard",
                      NULL,
                      NULL,
                      NULL,
                      NULL,
                      NULL
    };

    if (num_extra_volumes != 0) {
	    if (extra_path != NULL) {
	        list[2] = "Advanced backup to ExtraSD";
	        list[3] = "Advanced restore from ExtraSD";
	    }
	}
    if (usb_path != NULL && ensure_path_mounted(usb_path) == 0) {
		list[4] = "Advanced backup to USB-Drive";
		list[5] = "Advanced restore from USB-Drive";
	}

    for (;;) {
        int chosen_item = get_filtered_menu_selection(headers, list, 0, 0, sizeof(list) / sizeof(char*));
        if (chosen_item == GO_BACK || chosen_item == REFRESH)
            break;
		switch (chosen_item)
        	{	
			case 0:
				show_nandroid_advanced_backup_menu(primary_path);
                break;
            case 1:
                show_nandroid_advanced_restore_menu(primary_path);
                break;
            case 2:
				show_nandroid_advanced_backup_menu(extra_path);
                break;
            case 3:
				show_nandroid_advanced_restore_menu(extra_path);
                break;
            case 4:
				show_nandroid_advanced_backup_menu(usb_path);
                break;
            case 5:
				show_nandroid_advanced_restore_menu(usb_path);
                break;
            default:
#ifdef RECOVERY_EXTEND_NANDROID_MENU
                handle_nandroid_menu(6, chosen_item);
#endif
                break;    
          }
    }
    
}

void show_nandroid_menu() {
	char* primary_path = get_primary_storage_path();
    char* extra_path = get_extra_storage_path();
    char* usb_path = get_usb_storage_path();
    int num_extra_volumes = get_num_extra_volumes();
    
    const char* headers[] = { "Backup and Restore", NULL };

    char* list[] = { "BACKUP to SDcard",
					"RESTORE from SDcard",
					"DELETE Backup from SDcard",
					"ADVANCED Backup Restore",
					"Default backup format",
					"Toggle MD5 Verification",
					NULL,
					NULL,
					NULL,
					NULL,
					NULL,
					NULL,
					NULL
    };

    if (num_extra_volumes != 0) {
	    if (extra_path != NULL) {
			list[6] = "BACKUP to ExtraSD";
			list[7] = "RESTORE from ExtraSD";
			list[8] = "DELETE Backup from ExtraSD";
		}
	}
	if (usb_path != NULL && ensure_path_mounted(usb_path) == 0) {
		list[9] = "BACKUP to USB-Drive";
		list[10] = "RESTORE from USB-Drive";
		list[11] = "DELETE Backup from USB-Drive";
	}
#ifdef RECOVERY_EXTEND_NANDROID_MENU
    extend_nandroid_menu(list, 12, sizeof(list) / sizeof(char*));
#endif

    for (;;) {
        int chosen_item = get_filtered_menu_selection(headers, list, 0, 0, sizeof(list) / sizeof(char*));
        if (chosen_item == GO_BACK || chosen_item == REFRESH)
            break;
        switch (chosen_item)
        {
            case 0:
                {
                    char backup_path[PATH_MAX];
                    time_t t = time(NULL);
                    struct tm *tmp = localtime(&t);
                    if (tmp == NULL) {
                        struct timeval tp;
                        gettimeofday(&tp, NULL);
                        snprintf(backup_path, PATH_MAX, "%s/clockworkmod/backup/CTR-%ld", primary_path, tp.tv_sec);
                    } else {
                        char path_fmt[PATH_MAX];
                        strftime(path_fmt, PATH_MAX, "%F-%H-%M-%S", tmp);
                        snprintf(backup_path, PATH_MAX, "%s/clockworkmod/backup/CTR-%s", primary_path, path_fmt);
                    }
                    nandroid_backup(backup_path);
                }
                break;
            case 1:
				show_nandroid_restore_menu(primary_path);
				break;
            case 2:
				show_nandroid_delete_menu(primary_path);
				break;
            case 3:
                show_nandroid_advanced_menu();
                break;
            case 4:
                choose_default_backup_format();
                break;  
            case 5:
                toggle_md5_check();
                break;           
            case 6:
                {
                    char backup_path[PATH_MAX];
                    time_t t = time(NULL);
                    struct tm *tmp = localtime(&t);
                    if (tmp == NULL) {
                        struct timeval tp;
                        gettimeofday(&tp, NULL);
                        snprintf(backup_path, PATH_MAX, "%s/clockworkmod/backup/CTR-%ld", extra_path, tp.tv_sec);
                    } else {
                        char path_fmt[PATH_MAX];
                        strftime(path_fmt, PATH_MAX, "%F-%H-%M-%S", tmp);
                        snprintf(backup_path, PATH_MAX, "%s/clockworkmod/backup/CTR-%s", extra_path, path_fmt);
                    }
                    nandroid_backup(backup_path);
                }
                break;
            case 7:
				show_nandroid_restore_menu(extra_path);
                break;
            case 8:
				show_nandroid_delete_menu(extra_path);
                break;           
            case 9:
                {
                    char backup_path[PATH_MAX];
                    time_t t = time(NULL);
                    struct tm *tmp = localtime(&t);
                    if (tmp == NULL) {
                        struct timeval tp;
                        gettimeofday(&tp, NULL);
                        snprintf(backup_path, PATH_MAX, "%s/clockworkmod/backup/CTR-%ld", usb_path, tp.tv_sec);
                    } else {
                        char path_fmt[PATH_MAX];
                        strftime(path_fmt, PATH_MAX, "%F-%H-%M-%S", tmp);
                        snprintf(backup_path, PATH_MAX, "%s/clockworkmod/backup/CTR-%s", usb_path, path_fmt);
                    }
                    nandroid_backup(backup_path);
                }
                break;
            case 10:
				show_nandroid_restore_menu(usb_path);
                break;
            case 11:
				show_nandroid_delete_menu(usb_path);
                break;   
                    
            default:
#ifdef RECOVERY_EXTEND_NANDROID_MENU
                handle_nandroid_menu(12, chosen_item);
#endif
                break;
        }
    }
}

static int flash_choosen_image(const char* image, const char* partition) {
    Volume *vol = volume_for_path(partition);
    // make sure the volume exists...
    if (vol == NULL || vol->fs_type == NULL)
        return 0;

	int ret;
	const char* name = basename(partition);
	
	ui_print("[*] Erasing %s before flashing...\n", name);
	if (0 != (ret = format_volume(partition))) {
		ui_print("Error while erasing %s partition!", name);
		return ret;
	}

	ui_print("[*] Flashing %s image...\n", name);
	if (0 != (ret = restore_raw_partition(vol->fs_type, vol->device, image))) {
		ui_print("Error while flashing %s image!", name);
		return ret;
	}
	ui_print("\n[*] %s image flashed!\n", name);
	return 0;
}

static int ctr_flash_image(const char* image, int boot, int recovery) {

	ui_set_background(BACKGROUND_ICON_INSTALLING);
    ui_show_indeterminate_progress();
    const char* imgname = basename(image);

	int ret;
	char confirm[PATH_MAX];
    sprintf(confirm, "Yes - Flash it!");
	if (boot) {
		if(strstr(imgname, "boot")) { 
			if (0 != (ret = flash_choosen_image(image, "/boot"))) return ret;
		} else {
			if (confirm_selection("Are you sure this is a boot image?", confirm)) {	
			    if (0 != (ret = flash_choosen_image(image, "/boot"))) return ret;
			}
		}
		
	}
	
	if (recovery) {
		if(strstr(imgname, "recovery")) { 
			if (0 != (ret = flash_choosen_image(image, "/recovery"))) return ret;
		} else {
			if (confirm_selection("Are you sure this is a recovery image?", confirm)) {	
			    if (0 != (ret = flash_choosen_image(image, "/recovery"))) return ret;
			}
		}
		
	}

    sync();
    ui_reset_progress();
    ui_set_background(BACKGROUND_ICON_NONE);
    sleep(1);
	
	if (recovery && confirm_selection("Do you want to reboot recovery now?", "Yes - Reboot recovery")) {	
	    reboot_main_system(ANDROID_RB_RESTART2, 0, "recovery");
	    return 0;
	}

    ui_set_background(BACKGROUND_ICON_CLOCKWORK);
    return 0;
}

static void choose_image_path_menu(const char* image) {

    const char* headers[] = { "Choose the partition to flash on", NULL };
    
	char* partition_items[] = { "Boot",
								"Recovery",
								NULL };

	for (;;) {
		int chosen_item = get_menu_selection(headers, partition_items, 0, 0);
		if (chosen_item == GO_BACK || chosen_item == REFRESH)
            break;
		switch (chosen_item) {
		  case 0:
			ctr_flash_image(image, 1, 0);
			break;
		  case 1:
			ctr_flash_image(image, 0, 1);
			break;
		}
	}    
}

static void show_choose_image_menu(const char *mount_point) {
    if (ensure_path_mounted(mount_point) != 0) {
        LOGE("Can't mount %s\n", mount_point);
        return;
    }

    const char* headers[] = { "Choose an image to flash", NULL };

    char* file = choose_file_menu(mount_point, ".img", headers);
    if (file == NULL)
        return;
    char confirm[PATH_MAX];
    sprintf(confirm, "Yes - Flash %s", basename(file));

    if (confirm_selection("Confirm Flash this image?", confirm)) {
        choose_image_path_menu(file);
    }
    
    free(file);
}

static void show_flash_image_menu() {
	char* primary_path = get_primary_storage_path();
    char* extra_path = get_extra_storage_path();
    char* usb_path = get_usb_storage_path();
    int num_extra_volumes = get_num_extra_volumes();
    
    const char* headers[] = {  "Flash Images from", NULL };

    static char* list[] = { "SDcard",
                            NULL,
                            NULL,
                            NULL
    };

    if (num_extra_volumes != 0) {
	    if (extra_path != NULL)
	        list[1] = "ExtraSD";
	}
    if (usb_path != NULL && ensure_path_mounted(usb_path) == 0) {
        list[2] = "USB-Drive";
	}

    for (;;) {
        int chosen_item = get_filtered_menu_selection(headers, list, 0, 0, sizeof(list) / sizeof(char*));
        if (chosen_item == GO_BACK || chosen_item == REFRESH)
            break;
        switch (chosen_item)
        {
            case 0:
                show_choose_image_menu(primary_path);
                break;
            case 1:
                show_choose_image_menu(extra_path);
                break;
            case 2:
                show_choose_image_menu(usb_path);
                break;
        }
    }
}

void format_sdcard(const char* volume) {
    // this will also ensure it is not /data/media
    if (!can_partition(volume))
        return;
	if (is_data_media_volume_path(volume))
        return;
	
    Volume *v = volume_for_path(volume);    
	if (v == NULL || strcmp(v->fs_type, "auto") != 0)
        return;


    const char* headers[] = {"Format device:", volume, "", NULL };

    static char* list[] = { "default",
			        "ext2",
			        "ext3",
			        "ext4",
			        "vfat",
			        "exfat",
			        "ntfs",
					"f2fs",
					NULL
    };

    int ret = -1;
    char cmd[PATH_MAX];
    struct stat sv;
    ssize_t length = 0;
	if (v->length != 0) length = v->length;
    int chosen_item = get_menu_selection(headers, list, 0, 0);
    if (chosen_item == GO_BACK || chosen_item == REFRESH)
        return;
    if (!confirm_selection( "Confirm formatting?", "Yes - Format device"))
        return;
        
	if (ensure_path_unmounted(v->mount_point) != 0)
        return;

    switch (chosen_item)
    {
        case 0:
            ret = format_volume(v->mount_point);
            break;
        case 1:
            ret = format_unknown_device(v->device, v->mount_point, "ext2");
            break;
        case 2:
            ret = format_unknown_device(v->device, v->mount_point, "ext3");
            break;
        case 3:
	        ret = make_ext4fs(v->device, length, v->mount_point, sehandle);
            break;
        case 4:
            if (stat("/sbin/newfs_msdos", &sv) == 0) {
				sprintf(cmd, "/sbin/newfs_msdos -F 32 -O android -c 8 %s", v->device);
                ret = __system(cmd);
			}
            break;
        case 5:
            if (stat("/sbin/mkfs.exfat", &sv) == 0) {
				sprintf(cmd, "/sbin/mkfs.exfat %s", v->device);
                ret = __system(cmd);
			}
            break;
        case 6:
            if (stat("/sbin/mkfs.ntfs", &sv) == 0) {
				sprintf(cmd, "/sbin/mkfs.ntfs -f %s", v->device);
                ret = __system(cmd);
			}
            break;
        case 7:
            if (stat("/sbin/mkfs.f2fs", &sv) == 0) {
				if (length < 0) {
					LOGE("format_volume: negative length (%zd) not supported on %s\n", length, v->fs_type);
					return;
				}
				char *num_sectors;
				if (asprintf(&num_sectors, "%zd", length / 512) <= 0) {
					LOGE("format_volume: failed to create %s command for %s\n", v->fs_type, v->device);
					return;
				}
				const char *f2fs_path = "/sbin/mkfs.f2fs";
				const char* const f2fs_argv[] = {"mkfs.f2fs", "-t", "-d1", v->device, num_sectors, NULL};
		
				ret = exec_cmd(f2fs_path, (char* const*)f2fs_argv);
				free(num_sectors);
			}
            break;
    }

    if (ret)
        ui_print("Could not format %s (%s)\n", volume, list[chosen_item]);
    else
        ui_print("Done formatting %s (%s)\n", volume, list[chosen_item]);
}

int can_partition(const char* volume) {
    Volume *vol = volume_for_path(volume);
    if (vol == NULL) {
        LOGI("Can't format unknown volume: %s\n", volume);
        return 0;
    }

    if (!is_safe_to_format(volume)) {
		LOGI("Can't partition, format forbidden on: %s\n", volume);
		return 0;
	}
	
	if (is_data_media_volume_path(volume)) {
		LOGI("Can't partition, datamedia volume on: %s\n", volume);
		return 0;
	}

    int vol_len;
    const char *device = NULL;
    if (strstr(vol->device, "/dev/block/mmcblk") != NULL) {
        device = vol->device;
    } else if (vol->device2 != NULL && strstr(vol->device2, "/dev/block/mmcblk") != NULL) {
        device = vol->device2;
    } else {
        LOGI("Can't partition non mmcblk device: %s\n", vol->device);
        return 0;
    }

    vol_len = strlen(device);
    // do not allow partitioning of a device that isn't mmcblkX or mmcblkXp1
    if (device[vol_len - 2] == 'p' && device[vol_len - 1] != '1') {
        LOGI("Can't partition unsafe device: %s\n", device);
        return 0;
    }

    return 1;
}

static void partition_sdcard(char* volume) {
    if (!can_partition(volume)) {
        ui_print("Can't partition device: %s\n", volume);
        return;
    }

    static char* ext_sizes[] = { "128M",
                                 "256M",
                                 "512M",
                                 "1024M",
                                 "2048M",
                                 "4096M",
                                 NULL };

    static char* swap_sizes[] = { "0M",
                                  "32M",
                                  "64M",
                                  "128M",
                                  "256M",
                                  NULL };

    static char* partition_types[] = { "ext3",
                                       "ext4",
                                       NULL };

    static const char* ext_headers[] = { "Ext Size", NULL };
    static const char* swap_headers[] = { "Swap Size", NULL };
    static const char* fstype_headers[] = { "Partition Type", NULL };

    int ext_size = get_menu_selection(ext_headers, ext_sizes, 0, 0);
    if (ext_size == GO_BACK)
        return;

    int swap_size = get_menu_selection(swap_headers, swap_sizes, 0, 0);
    if (swap_size == GO_BACK)
        return;

    int partition_type = get_menu_selection(fstype_headers, partition_types, 0, 0);
    if (partition_type == GO_BACK)
        return;

    char sddevice[256];
    Volume *vol = volume_for_path(volume);
    if (strstr(vol->device, "/dev/block/mmcblk") != NULL)
        strcpy(sddevice, vol->device);
    else
        strcpy(sddevice, vol->device2);
    // we only want the mmcblk, not the partition
    sddevice[strlen("/dev/block/mmcblkX")] = '\0';
    char cmd[PATH_MAX];
    setenv("SDPATH", sddevice, 1);
    sprintf(cmd, "sdparted -es %s -ss %s -efs %s -s", ext_sizes[ext_size], swap_sizes[swap_size], partition_types[partition_type]);
    ui_print("Partitioning SD Card... please wait...\n");
    if (0 == __system(cmd))
        ui_print("Done!\n");
    else
        ui_print("An error occured while partitioning your SD Card. Please see /tmp/recovery.log for more details.\n");
}

void toggle_rainbow() {
    ui_get_rainbow_mode = !ui_get_rainbow_mode;
    ui_print("Rainbow Mode: %s\n", ui_get_rainbow_mode ? "Enabled" : "Disabled");
}

//=========================================/
//=      Aroma menu, original work        =/
//=           of sk8erwitskil             =/
//=        adapted by carliv@xda          =/
//=========================================/

static void choose_aromafm_menu(const char* aromafm_path) {
    if (ensure_path_mounted(aromafm_path) != 0) {
        LOGE("Can't mount %s\n", aromafm_path);
        return;
    }

    const char* headers[] = {  "Find aromafm.zip in selected", NULL };

    char* aroma_file = choose_file_menu(aromafm_path, "aromafm.zip", headers);
    if (aroma_file == NULL)
        return;
    static char* confirm_install  = "Confirm Run Aroma?";
    static char confirm[PATH_MAX];
    sprintf(confirm, "Yes - Run %s", basename(aroma_file));
    if (confirm_selection(confirm_install, confirm)) {
        ui_show_text(0);
        install_zip(aroma_file);
        ui_show_text(1);
    }
}

//Show custom aroma menu: manually browse sdcards for Aroma file manager
static void custom_aroma_menu() {
	char* primary_path = get_primary_storage_path();
    char* extra_path = get_extra_storage_path();
    char* usb_path = get_usb_storage_path();
    int num_extra_volumes = get_num_extra_volumes();
    
    const char* headers[] = {  "Browse Sdcards for aromafm", NULL };

    static char* list[] = { "Search SDcard",
                            NULL,
                            NULL,
                            NULL
    };

    if (num_extra_volumes != 0) {
	    if (extra_path != NULL)
	        list[1] = "Search ExtraSD";
	}
	if (usb_path != NULL && ensure_path_mounted(usb_path) == 0) {
        list[2] = "Search USB-Drive";
	}

    for (;;) {
        //header function so that "Toggle menu" doesn't reset to main menu on action selected
        int chosen_item = get_filtered_menu_selection(headers, list, 0, 0, sizeof(list) / sizeof(char*));
        if (chosen_item == GO_BACK || chosen_item == REFRESH)
            break;
        switch (chosen_item)
        {
            case 0:
                choose_aromafm_menu(primary_path);
                break;
            case 1:
                choose_aromafm_menu(extra_path);
                break;
            case 2:
                choose_aromafm_menu(usb_path);
                break;
        }
    }
}

//launch aromafm.zip from default locations
static int default_aromafm(const char* aromafm_path) {
	if (ensure_path_mounted(aromafm_path) != 0) {
	    return 0;
    }
    char aroma_file[PATH_MAX];
    sprintf(aroma_file, "%s/clockworkmod/.aromafm/aromafm.zip", aromafm_path);

    if (access(aroma_file, F_OK) != -1) {
        ui_show_text(0);
        install_zip(aroma_file);
        ui_show_text(1);
        return 1;
    } 
	return 0;
}

void show_carliv_menu() {
	char* primary_path = get_primary_storage_path();
    char* extra_path = get_extra_storage_path();
    int num_extra_volumes = get_num_extra_volumes();

    const char* headers[] = {  "Carliv Menu", NULL };

    char* carliv_list[] = { "Aroma File Manager",
							"Reset Battery Stats",
							"About",
							"Turn on/off Rainbow mode",
							"Flash boot or recovery images",
							NULL,
							NULL
    };
#ifdef BOARD_HAS_MTK_CPU
		carliv_list[5] = "Special MTK Partitions menu";
#endif    

    for (;;)
    {
		int chosen_item = get_menu_selection(headers, carliv_list, 0, 0);
        if (chosen_item == GO_BACK || chosen_item == REFRESH)
            break;
		switch (chosen_item)
        {
			case 0:
				{
                    ensure_path_mounted(primary_path);
                    if (default_aromafm(primary_path)) {
                        break;
	                }
	                if (num_extra_volumes != 0) {
		                if (extra_path != NULL) {
		                    ensure_path_mounted(extra_path);
		                    if (default_aromafm(extra_path)) {
		                        break;
		                    }
		                }
					}
	                ui_print("No clockworkmod/.aromafm/aromafm.zip on sdcards\n");
	                ui_print("Browsing custom locations\n");
	                custom_aroma_menu();
				}
                break;  
             case 1:                
                wipe_battery_stats(ui_text_visible());
				if (!ui_text_visible()) return;
				break;  
             case 2:
				ui_set_log_stdout(0);
				ui_clear_text();
				ui_set_background(BACKGROUND_ICON_NONE);
				ui_print(EXPAND(RECOVERY_VERSION)" ** "EXPAND(RECOVERY_BUILD_OS)" for "EXPAND(RECOVERY_DEVICE)"\n");
			    ui_print("Compiled by "EXPAND(RECOVERY_BUILD_USER)"@"EXPAND(RECOVERY_BUILD_HOST)" on: "EXPAND(RECOVERY_BUILD_DATE)"\n\n");
                ui_print("Based on Clockworkmod recovery.\n");
                ui_print("This is a Recovery made by carliv from xda with Clockworkmod base and many improvements inspired from TWRP, PhilZ  or created by carliv.\n");
                ui_print("With full touch support module developed by PhilZ for PhilZ Touch Recovery, ported here by carliv.\n");
				if (volume_for_path("/custpack") != NULL)
					ui_print("[*] With Custpack partition support for Alcatel or TCL phones\n");
				ui_print("For Aroma File Manager is recommended version 1.80 - Calung, from amarullz xda thread, because it has a full touch support in most of devices.\n");
				ui_print("Thank you all!\n\n");
				ui_print("Return to menu with any key.\n");
				ui_wait_key();
                ui_clear_key_queue();
                ui_clear_text();
                ui_set_background(BACKGROUND_ICON_CLOCKWORK);
                ui_set_log_stdout(1);
                break;                  
             case 3:
                toggle_rainbow();
				break;                  
             case 4:
                show_flash_image_menu();
				break;             
#ifdef BOARD_HAS_MTK_CPU
			case 5:
                show_mtk_special_backup_restore_menu();
                break;
#endif				
        }
    }    
}

void show_advanced_menu() {
	char* primary_path = get_primary_storage_path();
    char* extra_path = get_extra_storage_path();
    int num_extra_volumes = get_num_extra_volumes();

    const char* headers[] = { "Advanced Menu", NULL };

    static char* list[] = { "Report Error",
                            "Key Test",
                            "Show log",
                            "Clear Screen",
                            NULL,
                            NULL,
                            NULL,
                            NULL
    };
    
    if (primary_path != NULL) {
	    if (can_partition(primary_path)) 
		    list[4] = "Partition SDcard";
	}
    if (num_extra_volumes != 0) {
		if (extra_path != NULL) {
		    if (can_partition(extra_path)) 
			    list[5] = "Partition ExtraSD";
		}
	}
#ifdef ENABLE_LOKI
		list[6] = "Toggle loki support";
#endif

    for (;;)
    {
        int chosen_item = get_filtered_menu_selection(headers, list, 0, 0, sizeof(list) / sizeof(char*));
        if (chosen_item == GO_BACK || chosen_item == REFRESH)
            break;
        switch (chosen_item)
        {
            case 0:
                handle_failure();
                break;
            case 1:
            {
				ui_print("Outputting key codes.\n");
                ui_print("Go back to end debugging.\n");
                int key;
                int action;
                do {
                    key = ui_wait_key();
                    action = device_handle_key(key, 1);
                    ui_print("Key: %d\n", key);
                } while (action != GO_BACK);
                break;
			}
            case 2:
	            ui_set_background(BACKGROUND_ICON_NONE);
                ui_printlogtail(32);
                ui_wait_key();
                ui_clear_key_queue();
                ui_set_background(BACKGROUND_ICON_CLOCKWORK);
                break;
			case 3:
                ui_clear_text();
                ui_set_background(BACKGROUND_ICON_CLOCKWORK);
                break;
			case 4:
                partition_sdcard(primary_path);
                break;
			case 5:
                partition_sdcard(extra_path);
                break;			
#ifdef ENABLE_LOKI
            case 6:
                toggle_loki_support();
                break;
#endif		           
        }
    }
}

void write_fstab_root(char *path, FILE *file) {
    Volume *vol = volume_for_path(path);
    if (vol == NULL) {
        LOGW("Unable to get ctr.fstab info for %s during fstab generation!\n", path);
        return;
    }

    char device[200];
    if (vol->device[0] != '/')
        get_partition_device(vol->device, device);
    else
        strcpy(device, vol->device);

    fprintf(file, "%s ", device);
    fprintf(file, "%s ", path);
    fprintf(file, "%s rw\n", vol->fs_type2 != NULL && strcmp(vol->fs_type, "rfs") != 0 ? "auto" : vol->fs_type);
}

static void create_fstab() {
    struct stat info;
    __system("touch /etc/mtab");
    FILE *file = fopen("/etc/fstab", "w");
    if (file == NULL) {
        LOGW("Unable to create /etc/fstab!\n");
        return;
    }
    Volume *vol = volume_for_path("/boot");
    if (NULL != vol && strcmp(vol->fs_type, "mtd") != 0 && strcmp(vol->fs_type, "emmc") != 0 && strcmp(vol->fs_type, "bml") != 0)
    write_fstab_root("/boot", file);
    write_fstab_root("/cache", file);
    write_fstab_root("/data", file);
    if (volume_for_path("/datadata") != NULL)
         write_fstab_root("/datadata", file);
    if (volume_for_path("/internal_sd") != NULL)
         write_fstab_root("/internal_sd", file);
    write_fstab_root("/system", file);
    if (volume_for_path("/custpack") != NULL)
         write_fstab_root("/custpack", file);
	if (volume_for_path("/cust") != NULL)
         write_fstab_root("/cust", file);
    write_fstab_root("/sdcard", file);
    if (volume_for_path("/sd-ext") != NULL)
         write_fstab_root("/sd-ext", file);
    if (volume_for_path("/external_sd") != NULL)
         write_fstab_root("/external_sd", file);
    if (volume_for_path("/usb-otg") != NULL)
         write_fstab_root("/usb-otg", file);
    if (volume_for_path("/usbdisk") != NULL)
         write_fstab_root("/usbdisk", file);
    fclose(file);
    LOGI("Completed outputting fstab.\n");
}

static int bml_check_volume(const char *path) {
    ui_print("Checking %s...\n", path);
    ensure_path_unmounted(path);
    if (0 == ensure_path_mounted(path)) {
        ensure_path_unmounted(path);
        return 0;
    }

    Volume *vol = volume_for_path(path);
    if (vol == NULL) {
        LOGE("Unable process volume! Skipping...\n");
        return 0;
    }

    ui_print("%s may be rfs. Checking...\n", path);
    char tmp[PATH_MAX];
    sprintf(tmp, "mount -t rfs %s %s", vol->device, path);
    int ret = __system(tmp);
    printf("%d\n", ret);
    return ret == 0 ? 1 : 0;
}

int has_datadata() {
    Volume *vol = volume_for_path("/datadata");
    if (vol == NULL) {
        return 0;
    }
    return 1;
}

void process_volumes() {
    create_fstab();
   
#ifdef BOARD_INCLUDE_CRYPTO
	if (ensure_path_mounted("/data") != 0) {
		ui_set_background(BACKGROUND_ICON_INSTALLING);
		encrypted_data_mounted = 0;
		set_encryption_state(1);
		setup_encrypted_data();
	} else {
		set_encryption_state(0);
		if (is_data_media()) setup_data_media();
	}	
#else
	set_encryption_state(0);
	if (is_data_media()) setup_data_media();	
#endif
    return;
}

void handle_failure() {
    if (0 != ensure_path_mounted(get_primary_storage_path()))
        return;
        
	char* primary_path = get_primary_storage_path();
	char tmp[PATH_MAX];
	char log[PATH_MAX];
	sprintf(log, "%s/clockworkmod", primary_path);
    mkdir(log, S_IRWXU | S_IRWXG | S_IRWXO);
    sprintf(tmp, "touch %s/recovery.log ; cp /tmp/recovery.log %s/recovery.log", log, log);
    __system(tmp);
    ui_print("\nDone!\n> recovery.log was copied to your main storage as clockworkmod/recovery.log. Please report the issue on recovery thread where you found it.\n");
}

int is_path_mounted(const char* path) {
    Volume* v = volume_for_path(path);
    if (v == NULL) {
        return 0;
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // the ramdisk is always mounted.
        return 1;
    }

    if (scan_mounted_volumes() < 0) {
        LOGE("failed to scan mounted volumes\n");
        return 0;
    }

    const MountedVolume* mv = find_mounted_volume_by_mount_point(v->mount_point);
    if (mv) {
        // volume is already mounted
        return 1;
    }
    return 0;
}

int volume_main(int argc, char **argv) {
    load_volume_table();
    return 0;
}

int verify_root_and_recovery() {
    if (ensure_path_mounted("/system") != 0)
        return 0;

    ui_set_background(BACKGROUND_ICON_CLOCKWORK);
    int ret = 0;
    struct stat st;
    if (0 == lstat("/system/recovery-from-boot.p", &st)) {
        ui_show_text(1);        
        if (confirm_selection("ROM may flash stock recovery on boot. Fix?", "Yes")) {
            __system("rm -rf /system/recovery-from-boot.p");
            ret = 1;
        }
    }

    if (0 != lstat("/system/etc/.installed_su_daemon", &st)) {
        if (0 == lstat("/system/etc/install-recovery.sh", &st)) {
            if (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) {
		        ui_show_text(1);		        
		        if (confirm_selection("ROM may flash stock recovery on boot. Fix?", "Yes - Disable recovery flash")) {
                    __system("chmod -x /system/etc/install-recovery.sh");
                    ret = 1;
                }
	        }
	    }
	}
    ensure_path_unmounted("/system");
    return ret;
}
