#include "util.h"
#include <cstdio>
#include <cstdlib>

void errif(bool condition, const char *message) {
  if (condition) {
    perror(message);
    perror("\n");
    exit(EXIT_FAILURE);
  }
}