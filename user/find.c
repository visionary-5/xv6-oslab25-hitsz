// user/find.c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "user/user.h"

// 取路径的最后一段（去掉'/...'之前的部分），返回到静态缓冲，保证以'\0'结尾
static char*
basename_trim(char *path) {
  static char buf[DIRSIZ + 1];
  char *p = path + strlen(path) - 1;

  // 去掉末尾可能的'/'
  while (p >= path && *p == '/')
    p--;
  if (p < path) {
    // path 全是'/'，约定返回"/"
    buf[0] = '/';
    buf[1] = 0;
    return buf;
  }

  // 找到前一个'/'
  char *start = p;
  while (start >= path && *start != '/')
    start--;
  start++; // 指向最后一段名的起始

  int len = (int)(p - start + 1);
  if (len > DIRSIZ) len = DIRSIZ;

  memmove(buf, start, len);
  buf[len] = 0;
  return buf;
}

// 打印匹配到的完整路径
static void
print_match(char *fullpath) {
  printf("%s\n", fullpath);
}

// 递归查找
static void
do_find(char *path, char *target) {
  int fd;
  struct stat st;

  if ((fd = open(path, 0)) < 0) {
    // 不可达就忽略
    // fprintf(2, "find: cannot open %s\n", path);
    return;
  }
  if (fstat(fd, &st) < 0) {
    // fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  if (st.type == T_FILE) {
    // 文件：比较文件名是否匹配
    if (strcmp(basename_trim(path), target) == 0) {
      print_match(path);
    }
    close(fd);
    return;
  }

  if (st.type == T_DIR) {
    struct dirent de;
    char buf[512], *p;

    // 预留空间：path + '/' + name(DIRSIZ) + '\0'
    if (strlen(path) + 1 + DIRSIZ + 1 > sizeof(buf)) {
      // 路径过长，跳过
      close(fd);
      return;
    }

    strcpy(buf, path);
    p = buf + strlen(buf);
    // 确保目录后面接一个'/'
    if (p == buf || *(p - 1) != '/')
      *p++ = '/';

    while (read(fd, &de, sizeof(de)) == sizeof(de)) {
      if (de.inum == 0) continue;

      // 目录项名字可能不是以'\0'结尾，需要拷到临时缓冲
      char name[DIRSIZ + 1];
      memmove(name, de.name, DIRSIZ);
      name[DIRSIZ] = 0;

      // 跳过 "." 和 ".."
      if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

      // 拼接新的完整路径：buf = path + "/" + name
      char *q = p;
      int nlen = strlen(name);
      memmove(q, name, nlen);
      q[nlen] = 0;

      // 对该项 stat 决定是否递归
      struct stat st2;
      if (stat(buf, &st2) < 0) {
        // fprintf(2, "find: cannot stat %s\n", buf);
        continue;
      }

      // 名字匹配就打印
      if (strcmp(name, target) == 0) {
        print_match(buf);
      }

      // 目录则递归进入
      if (st2.type == T_DIR) {
        do_find(buf, target);
      }
      // 恢复 buf 到 "path/" 的形态，q 指向追加处，直接覆盖即可（下一轮会重写）
      // 这里不需要显式恢复，因为每轮都会覆盖 q 开始的内容并重新写 '\0'
    }

    close(fd);
    return;
  }

  // 其他类型忽略
  close(fd);
}

int
main(int argc, char *argv[]) {
  if (argc != 3) {
    fprintf(2, "usage: find <path> <name>\n");
    exit(1);
  }
  do_find(argv[1], argv[2]);
  exit(0);
}
