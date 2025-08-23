#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <utime.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define LOG_BUF_SIZE 256
#define EXPAND_BUF_SIZE 64

#ifndef S_IALLUGO
#define S_IALLUGO (S_IRUSR | S_IRGRP | S_IROTH | \
                    S_IWUSR | S_IWGRP | S_IWOTH | \
                    S_IXUSR | S_IXGRP | S_IXOTH | \
                    S_ISUID | S_ISGID | S_ISVTX)
#endif // !S_IALLUGO

typedef int (*mkdir_cb)(const char* dir);

static const char* strnstr(const char* str, size_t n, const char* pattern)
{
    const size_t len = strlen(pattern);
    for (int i = 0; i <= (int)n - (int)len; ++i) {
        if (strncmp(str + i, pattern, len) == 0) {
            return str + i;
        }
    }

    return NULL;
}

static void error_msg(const char* format, ...)
{
    char* buf;
    size_t buf_size = LOG_BUF_SIZE;
    va_list args;

    buf = (char *)malloc(LOG_BUF_SIZE);
    while (true) {
        va_start(args, format);
        if (vsnprintf(buf, buf_size, format, args) < (int)buf_size) {
            break;
        }

        buf_size += EXPAND_BUF_SIZE;
        buf = (char *)realloc(buf, buf_size);
        va_end(args);
    }

    perror(buf);
    free(buf);
}

static int mkdir_p(const char* path, int mode, const mkdir_cb cb)
{
    char dir[PATH_MAX];
    const char* p = path, * q;
    char* j = dir;
    size_t n;
    bool is_abs;
    int err;

    if (!path || path[0] == '\0') {
        error_msg("Calling mkdir_p with empty path argument.");
        return -1;
    }

    is_abs = path[0] == '/';
    while ((q = strstr(p + is_abs, "/"))) {
        n = q - p;
        memcpy(j, p, n);
        j[n] = '\0';
        j += n;
        p = q;
        if (mkdir(dir, mode) && errno != EEXIST) {
            error_msg("Failed to mkdir %s", dir);
            return -1;
        }

        if (cb && (err = cb(dir)))
            return -1;
    }

    if (*++p != '\0') {
        if (mkdir(path, mode) && errno != EEXIST) {
            error_msg("Failed to mkdir %s", path);
            return -1;
        }
    }

    return 0;
}

static const char* append_path(const char* dir, const char* name)
{
    size_t n = strlen(dir), m = strlen(name);
    char* buf = (char*)malloc(n + m + 2);

    memcpy(buf, dir, n);
    buf[n] = '/';
    memcpy(buf + n + 1, name, m);
    buf[n + m + 1] = '\0';
    return buf;
}

typedef int (*read_dir_op)(const char* dir, const char* name, struct dirent* ent);

int read_dir(const char* path, const read_dir_op op)
{
    DIR* dir;
    struct dirent* ent;
    int err;

    if ((dir = opendir(path)) == NULL) {
        error_msg("Failed to open dir %s", path);
        err = -1;
        goto out1;
    }

    while ((ent = readdir(dir))) {
        (void)op(path, ent->d_name, ent);
        errno = 0;
    }

    if (errno) {
        error_msg("Failed to read dir %s", path);
        err = -1;
    }

    closedir(dir);
out1:
    return err;
}

int file_op_mkdir_cb(const char* dir)
{
    char src_dir[PATH_MAX];
    struct stat st;
    struct utimbuf timebuf;
    if (strncmp(dir, "/tmp", sizeof("/tmp") - 1)) {
        fprintf(stderr, "dir %s is not in \"/tmp\".\n", dir);
        return -1;
    }

    strcpy(src_dir, dir + sizeof("/tmp") - 1);
    if (src_dir[0] == '\0')
        return 0;

    if (stat(src_dir, &st)) {
        error_msg("Failed to stat directory %s", src_dir);
        return -1;
    }

    if (!S_ISDIR(st.st_mode))
        return 0;

    if (chmod(dir, st.st_mode & 07777))
        error_msg("Failed to chmod for directory %s", dir);

    if (chown(dir, st.st_uid, st.st_gid))
        error_msg("Failed to chown for directory %s", dir);

    timebuf.actime = st.st_atime;
    timebuf.modtime = st.st_mtime;
    if (utime(dir, &timebuf)) {
        error_msg("Failed to change file %s's access and modification times", dir);
        return -1;
    }

    return 0;
}

int copy_sparse_file(const char* filename, void* dest, void* src, size_t size, int src_fd)
{
    off_t data_start = 0, hole_start;

    if (lseek(src_fd, 0, SEEK_SET) < 0) {
        error_msg("Failed to seek file %s", filename);
        return -1;
    }

    errno = 0;
    while ((data_start = lseek(src_fd, data_start, SEEK_DATA)) >= 0) {
        hole_start = lseek(src_fd, data_start, SEEK_HOLE);
        if (hole_start < 0) {
            if (errno == ENXIO) {
                error_msg("File %s has changed during copy", filename);
                return -1;
            }

            memcpy(dest + data_start, src + data_start, size - data_start);
            data_start = size;
        } else {
            memcpy(dest + data_start, src + data_start, hole_start - data_start);
            data_start = hole_start;
        }
    }

    if (errno != ENXIO) {
        error_msg("Failed to seek file %s", filename);
        return -1;
    }

    return 0;
}

int file_op(const char* dir, const char* name, struct dirent* ent)
{
    const char* path, * dest_path, * dest_dir;
    void* map, * dest_map;
    int fd, dest_fd, err = 0;
    struct stat st;
    struct utimbuf timebuf;

    if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
        return 0;

    path = append_path(dir, name);
    memset((void *)&st, 0, sizeof(struct stat));
    if (stat(path, &st)) {
        error_msg("Failed to stat file %s", path);
        err = -1;
        goto out1;
    }

    if (S_ISDIR(st.st_mode)) {
        err = read_dir(path, &file_op);
        goto out1;
    }

    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "Path '%s' is not a regular file.\n", path);
        goto out1;
    }

    if ((fd = open(path, O_RDONLY, 0)) < 0) {
        error_msg("Failed to open file %s", path);
        err = -1;
        goto out1;
    }

    if (st.st_size <= 0)
        goto out2;

    if ((map = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0)) == MAP_FAILED) {
        error_msg("Failed to mmap file %s", path);
        err = -1;
        goto out2;
    }

    if (strnstr((const char*)map, st.st_size, "password")) {
        dest_path = append_path("/tmp", path + 1);
        dest_dir = append_path("/tmp", dir + 1);
        if ((err = mkdir_p(dest_dir, 0644, file_op_mkdir_cb)))
            goto out3;

        fprintf(stdout, "Copy file from %s to %s\n", path, dest_path);
        if ((dest_fd = open(dest_path, O_RDWR | O_CREAT | O_TRUNC, (st.st_mode & S_IALLUGO))) < 0) {
            error_msg("Failed to open dest file %s", dest_path);
            err = -1;
            goto out3;
        }

        if (ftruncate(dest_fd, st.st_size)) {
            error_msg("Failed to truncate dest file %s", dest_path);
            err = -1;
            goto out4;
        }

        if (posix_fadvise(dest_fd, 0, st.st_size, POSIX_FADV_DONTNEED))
            error_msg("Failed to advise the kernel about the likely sequential pattern to access to %s", dest_path);

        if ((dest_map = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, dest_fd, 0)) == MAP_FAILED) {
            error_msg("Failed to mmap dest file %s", dest_path);
            err = -1;
            goto out4;
        }

        if ((long long)st.st_blocks * st.st_blksize < st.st_size) {
            if ((err = copy_sparse_file(dest_path, dest_map, map, st.st_size, fd))) {
                goto out5;
            }
        } else {
            memcpy(dest_map, map, st.st_size);
        }

out5:
        munmap(dest_map, st.st_size);
out4:
        close(dest_fd);

        timebuf.actime = st.st_atime;
        timebuf.modtime = st.st_mtime;
        if (utime(dest_path, &timebuf))
            error_msg("Failed to change file %s's access and modification times", dest_path);

out3:
        free((void *)dest_dir);
        free((void *)dest_path);
    }

    munmap(map, st.st_size);
out2:
    close(fd);
out1:
    free((void *)path);

    return err;
}

int main(int /* argc */, char* /* argv */[])
{
    return read_dir("/etc", &file_op);
}

// vim: set et sw=4 ts=4 sts=4:
