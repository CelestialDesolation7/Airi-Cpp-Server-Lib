#include "util.h"
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>

void errif(bool condition, const char *message) {
  if (condition) {
    perror(message);
    perror("\n");
    exit(EXIT_FAILURE);
  }
}

// 辅助函数：设置文件描述符为非阻塞模式
void setnonblocking(int fd) {
  int oldoptions = fcntl(fd, F_GETFL);
  int new_option = oldoptions | O_NONBLOCK;
  fcntl(fd, F_SETFL, new_option);
}