/*
 * fat_fuse_ops.c
 *
 * FAT32 filesystem operations for FUSE (Filesystem in Userspace)
 */

#include "fat_fuse_ops.h"

#include "big_brother.h"
#include "fat_file.h"
#include "fat_filename_util.h"
#include "fat_fs_tree.h"
#include "fat_util.h"
#include "fat_volume.h"
#include <alloca.h>
#include <errno.h>
#include <gmodule.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define LOG_MESSAGE_SIZE 100
#define DATE_MESSAGE_SIZE 30
#define LOG_FILE_NAME LOG_FILE_BASENAME "." LOG_FILE_EXTENSION

// ----------------------------- LOG FUNCTIONS -----------------------------
static void fat_fuse_read_children(fat_tree_node dir_node);

static void fat_fuse_log_init() {
    fat_volume vol = get_fat_volume();
    fat_tree_node file_node = NULL;
    int error = 0;

    // BB directory
    fat_tree_node dir_node = fat_tree_node_search(vol->file_tree, BB_DIRNAME);
    if (dir_node) { // ==> LOG File exists
        DEBUG("BB Directory exists ==> LOG File must to exists");

        fat_fuse_read_children(dir_node); // Refresh BB Directory

        file_node = fat_tree_node_search(vol->file_tree, BB_LOG_FILE);
        if (file_node)
            DEBUG("LOG File already exists");
        else
            DEBUG("ERROR --> LOG File doesn't exists");

        return;
    }

    DEBUG("BB Directory doesn't exists ==> LOG File doesn't exists");

    error = fat_fuse_mkdir(BB_DIRNAME, 0);
    if (error)
        return;
    error = fat_fuse_mknod(BB_LOG_FILE, 0, 0);
    if (error)
        return;

    file_node = fat_tree_node_search(vol->file_tree, BB_LOG_FILE);
    if (file_node)
        DEBUG("LOG File successfully created");
    else
        DEBUG("ERROR --> LOG File, bad creation");
}

static void fat_fuse_log_write(char *buf, size_t size) {
    fat_volume vol = get_fat_volume();
    fat_tree_node file_node = fat_tree_node_search(vol->file_tree, BB_LOG_FILE);
    fat_file file = fat_tree_get_file(file_node);
    fat_file parent = fat_tree_get_parent(file_node);
    off_t offset = file->dentry->file_size;

    if (size == 0)
        DEBUG("LOG Write error --> Nothing to write");

    fat_file_pwrite(file, buf, size, offset, parent);
}

static void now_to_str(char *buf) {
    time_t now = time(NULL);
    struct tm *timeinfo;
    timeinfo = localtime(&now);

    strftime(buf, DATE_MESSAGE_SIZE, "%d-%m-%Y %H:%M", timeinfo);
}

static void fat_fuse_log_activity(char *operation_type, fat_file file) {
    char buf[LOG_MESSAGE_SIZE] = "";
    now_to_str(buf);
    strcat(buf, "\t");
    strcat(buf, getlogin());
    strcat(buf, "\t");
    strcat(buf, file->filepath);
    strcat(buf, "\t");
    strcat(buf, operation_type);
    strcat(buf, "\n");
    int message_size = strlen(buf);

    fat_fuse_log_write(buf, message_size);
}

// -------------------------- END OF LOG FUNCTIONS --------------------------

/* Get file attributes (file descriptor version) */
int fat_fuse_fgetattr(const char *path, struct stat *stbuf,
                      struct fuse_file_info *fi) {
    fat_file file = (fat_file)fat_tree_get_file((fat_tree_node)fi->fh);
    fat_file_to_stbuf(file, stbuf);
    return 0;
}

/* Get file attributes (path version) */
int fat_fuse_getattr(const char *path, struct stat *stbuf) {
    fat_volume vol;
    fat_file file;

    vol = get_fat_volume();
    file = fat_tree_search(vol->file_tree, path);
    if (file == NULL) {
        errno = ENOENT;
        return -errno;
    }
    fat_file_to_stbuf(file, stbuf);
    return 0;
}

/* Open a file */
int fat_fuse_open(const char *path, struct fuse_file_info *fi) {
    fat_volume vol;
    fat_tree_node file_node;
    fat_file file;

    vol = get_fat_volume();
    file_node = fat_tree_node_search(vol->file_tree, path);
    if (!file_node)
        return -errno;
    file = fat_tree_get_file(file_node);
    if (fat_file_is_directory(file))
        return -EISDIR;
    fat_tree_inc_num_times_opened(file_node);
    fi->fh = (uintptr_t)file_node;
    return 0;
}

/* Open a directory */
int fat_fuse_opendir(const char *path, struct fuse_file_info *fi) {
    fat_volume vol = NULL;
    fat_tree_node file_node = NULL;
    fat_file file = NULL;

    vol = get_fat_volume();
    file_node = fat_tree_node_search(vol->file_tree, path);
    if (file_node == NULL) {
        return -errno;
    }
    file = fat_tree_get_file(file_node);
    if (!fat_file_is_directory(file)) {
        return -ENOTDIR;
    }
    fat_tree_inc_num_times_opened(file_node);
    fi->fh = (uintptr_t)file_node;
    return 0;
}

/* Read directory children. Calls function fat_file_read_children which returns
 * a list of files inside a GList. The children were read from the directory
 * entries in the cluster of the directory.
 * This function iterates over the list of children and adds them to the
 * file tree.
 * This operation should be performed only once per directory, the first time
 * readdir is called.
 */
static void fat_fuse_read_children(fat_tree_node dir_node) {
    fat_volume vol = get_fat_volume();
    fat_file dir = fat_tree_get_file(dir_node);
    GList *children_list = fat_file_read_children(dir);
    // Add child to tree. TODO handle duplicates
    for (GList *l = children_list; l != NULL; l = l->next) {
        vol->file_tree =
            fat_tree_insert(vol->file_tree, dir_node, (fat_file)l->data);
    }
}

/* Add entries of a directory in @fi to @buf using @filler function. */
int fat_fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                     off_t offset, struct fuse_file_info *fi) {
    errno = 0;
    fat_tree_node dir_node = (fat_tree_node)fi->fh;
    fat_file dir = fat_tree_get_file(dir_node);
    fat_file *children = NULL, *child = NULL;
    int error = 0;

    // Insert first two filenames (. and ..)
    if ((*filler)(buf, ".", NULL, 0) || (*filler)(buf, "..", NULL, 0)) {
        return -errno;
    }
    if (!fat_file_is_directory(dir)) {
        errno = ENOTDIR;
        return -errno;
    }
    if (dir->children_read != 1) {
        fat_fuse_read_children(dir_node);
        if (errno < 0) {
            return -errno;
        }
    }

    children = fat_tree_flatten_h_children(dir_node);
    child = children;
    while (*child != NULL) {
        // BB Vigilance (It's a tag to search the new code)
        if (!strcmp((*child)->name, LOG_FILE_NAME)) {
            DEBUG("Hide File Name --> %s", (*child)->name);
            child++;
            continue;
        }

        error = (*filler)(buf, (*child)->name, NULL, 0);
        if (error != 0) {
            return -errno;
        }
        child++;
    }

    // BB Vigilance (It's a tag to search the new code)
    fat_fuse_log_init();

    return 0;
}

/* Read data from a file */
int fat_fuse_read(const char *path, char *buf, size_t size, off_t offset,
                  struct fuse_file_info *fi) {
    errno = 0;
    int bytes_read;
    fat_tree_node file_node = (fat_tree_node)fi->fh;
    fat_file file = fat_tree_get_file(file_node);
    fat_file parent = fat_tree_get_parent(file_node);

    bytes_read = fat_file_pread(file, buf, size, offset, parent);
    if (errno != 0) {
        return -errno;
    }

    // BB Vigilance (It's a tag to search the new code)
    fat_fuse_log_activity("READ", file);

    return bytes_read;
}

/* Write data from a file */
int fat_fuse_write(const char *path, const char *buf, size_t size, off_t offset,
                   struct fuse_file_info *fi) {
    fat_tree_node file_node = (fat_tree_node)fi->fh;
    fat_file file = fat_tree_get_file(file_node);
    fat_file parent = fat_tree_get_parent(file_node);

    if (size == 0)
        return 0; // Nothing to write
    if (offset > file->dentry->file_size)
        return -EOVERFLOW;

    // BB Vigilance (It's a tag to search the new code)
    fat_fuse_log_activity("WRITE", file);

    return fat_file_pwrite(file, buf, size, offset, parent);
}

/* Close a file */
int fat_fuse_release(const char *path, struct fuse_file_info *fi) {
    fat_tree_node file = (fat_tree_node)fi->fh;
    fat_tree_dec_num_times_opened(file);
    return 0;
}

/* Close a directory */
int fat_fuse_releasedir(const char *path, struct fuse_file_info *fi) {
    fat_tree_node file = (fat_tree_node)fi->fh;
    fat_tree_dec_num_times_opened(file);
    return 0;
}

int fat_fuse_mkdir(const char *path, mode_t mode) {
    errno = 0;
    fat_volume vol = NULL;
    fat_file parent = NULL, new_file = NULL;
    fat_tree_node parent_node = NULL;

    // The system has already checked the path does not exist. We get the parent
    vol = get_fat_volume();
    parent_node = fat_tree_node_search(vol->file_tree, dirname(strdup(path)));
    if (parent_node == NULL) {
        errno = ENOENT;
        return -errno;
    }
    parent = fat_tree_get_file(parent_node);
    if (!fat_file_is_directory(parent)) {
        fat_error("Error! Parent is not directory\n");
        errno = ENOTDIR;
        return -errno;
    }

    // init child
    new_file = fat_file_init(vol->table, true, strdup(path));
    if (errno != 0) {
        return -errno;
    }
    // insert to directory tree representation
    vol->file_tree = fat_tree_insert(vol->file_tree, parent_node, new_file);
    // write file in parent's entry (disk)
    fat_file_dentry_add_child(parent, new_file);
    return -errno;
}

/* Creates a new file in @path. @mode and @dev are ignored. */
int fat_fuse_mknod(const char *path, mode_t mode, dev_t dev) {
    errno = 0;
    fat_volume vol;
    fat_file parent, new_file;
    fat_tree_node parent_node;

    // The system has already checked the path does not exist. We get the parent
    vol = get_fat_volume();
    parent_node = fat_tree_node_search(vol->file_tree, dirname(strdup(path)));
    if (parent_node == NULL) {
        errno = ENOENT;
        return -errno;
    }
    parent = fat_tree_get_file(parent_node);
    if (!fat_file_is_directory(parent)) {
        fat_error("Error! Parent is not directory\n");
        errno = ENOTDIR;
        return -errno;
    }
    new_file = fat_file_init(vol->table, false, strdup(path));
    if (errno < 0) {
        return -errno;
    }
    // insert to directory tree representation
    vol->file_tree = fat_tree_insert(vol->file_tree, parent_node, new_file);
    // Write dentry in parent cluster
    fat_file_dentry_add_child(parent, new_file);
    return -errno;
}

int fat_fuse_utime(const char *path, struct utimbuf *buf) {
    errno = 0;
    fat_file parent = NULL;
    fat_volume vol = get_fat_volume();
    fat_tree_node file_node = fat_tree_node_search(vol->file_tree, path);
    if (file_node == NULL || errno != 0) {
        errno = ENOENT;
        return -errno;
    }
    parent = fat_tree_get_parent(file_node);
    if (parent == NULL || errno != 0) {
        DEBUG("WARNING: Setting time for parent ignored");
        return 0; // We do nothing, no utime for parent
    }
    fat_utime(fat_tree_get_file(file_node), parent, buf);
    return -errno;
}

/* Shortens the file at the given offset.*/
int fat_fuse_truncate(const char *path, off_t offset) {
    errno = 0;
    fat_volume vol = get_fat_volume();
    fat_file file = NULL, parent = NULL;
    fat_tree_node file_node = fat_tree_node_search(vol->file_tree, path);
    if (file_node == NULL || errno != 0) {
        errno = ENOENT;
        return -errno;
    }
    file = fat_tree_get_file(file_node);
    if (fat_file_is_directory(file))
        return -EISDIR;

    parent = fat_tree_get_parent(file_node);
    fat_tree_inc_num_times_opened(file_node);
    fat_file_truncate(file, offset, parent);
    return -errno;
}
