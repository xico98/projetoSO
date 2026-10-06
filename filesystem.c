#define _XOPEN_SOURCE 700

#include "filesystem.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include "constants.h"

int path_exists(const char *path){
  struct stat st;

  if (stat(path, &st) != 0)
    return 0;

  return S_ISDIR(st.st_mode);
}

int file_exists(const char *path){
  struct stat st;

  if (stat(path, &st) != 0)
    return 0;

  return S_ISREG(st.st_mode);
}

int absolute_path(const char *path, char *buffer, size_t size){
  char *resolved = realpath(path, NULL);

  if (resolved == NULL)
    return 1;

  if (strlen(resolved) >= size) {
    free(resolved);
    return 1;
  }

  strcpy(buffer, resolved);

  free(resolved);
  return 0;
}

static int copy_file(const char *src, const char *dst) {
  int in = open(src, O_RDONLY);
  if (in == -1)
    return 1;

  int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out == -1) {
    close(in);
    return 1;
  }

  char buf[4096];
  ssize_t r;

  while ((r = read(in, buf, sizeof(buf))) > 0) {
    ssize_t written = 0;
    while (written < r) {
      ssize_t w = write(out, buf + written, (size_t)(r - written));
      if (w == -1) {
        close(in);
        close(out);
        return 1;
      }
      written += w;
    }
  }

  close(in);
  close(out);
  return (r == -1) ? 1 : 0;
}


int copy_directory(const char *src, const char *dst){
  DIR *dir = opendir(src);
  if (dir == NULL){
    return 1;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL){
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0){
      continue;
    }

    char src_path[MAX_PATH_SIZE];
    int n = snprintf(src_path, sizeof(src_path), "%s/%s", src, entry->d_name);

    if (n < 0 || (size_t)n >= sizeof(src_path)){
      closedir(dir);
      return 1;
    }

    char dst_path[MAX_PATH_SIZE];
    n = snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, entry->d_name);

    if (n < 0 || (size_t)n >= sizeof(dst_path)){
      closedir(dir);
      return 1;
    }

    struct stat st;
    if (stat(src_path, &st) != 0){
      closedir(dir);
      return 1;
    }
    if (S_ISDIR(st.st_mode)){
      if (mkdir(dst_path, 0755) == -1 && errno != EEXIST){
        closedir(dir);
        return 1;
      }
      if (copy_directory(src_path, dst_path) != 0){
        closedir(dir);
        return 1;
      }
    } else if (S_ISREG(st.st_mode)){
      if (copy_file(src_path, dst_path) != 0){
        closedir(dir);
        return 1;
      }
    }
  }

  closedir(dir);
  return 0;
}