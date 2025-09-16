#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

void find(char *path, char *filename) {
  char buf[512], *p;
  int fd;
  struct dirent de;
  struct stat st;

  if ((fd = open(path, 0)) < 0) {
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }

  if (fstat(fd, &st) < 0) {
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  switch (st.type) {
    case T_FILE:
      // 只比较文件名部分
      char *base = path + strlen(path);
      while (base > path && *(base - 1) != '/') base--;
      if (strcmp(base, filename) == 0) {
        printf("%s\n", path);
      }
      break;

    case T_DIR:
      if (strlen(path) + 1 + DIRSIZ + 1 > sizeof buf) {
        printf("find: path too long\n");
        break;
      }
      strcpy(buf, path);
      p = buf + strlen(buf);
      *p++ = '/';
      while (read(fd, &de, sizeof(de)) == sizeof(de)) {
        if (de.inum == 0) continue;
        // 跳过 . 和 ..
        if (de.name[0] == '.' && (de.name[1] == 0 || (de.name[1] == '.' && de.name[2] == 0))) continue;
        memmove(p, de.name, DIRSIZ);
        p[DIRSIZ] = 0;
        if (stat(buf, &st) < 0) {
          printf("find: cannot stat %s\n", buf);
          continue;
        }
        if (st.type == T_FILE && strcmp(de.name, filename) == 0) {
          printf("%s\n", buf);
        }
        if (st.type == T_DIR) {
          if (strcmp(de.name, filename) == 0) {
            printf("%s\n", buf);
          }
          find(buf, filename);
        }
      }
      break;
  }
  close(fd);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printf("Usage: find <directory> <filename>\n");
    exit(0);
  }
  find(argv[1], argv[2]);
  exit(0);
}