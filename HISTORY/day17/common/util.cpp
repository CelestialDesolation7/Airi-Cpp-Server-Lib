#include "util.h"
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>

void ErrIf(bool condition, const char *message) {
    if (condition) {
        perror(message);
        exit(EXIT_FAILURE);
    }
}