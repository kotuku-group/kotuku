#pragma once

#include <kotuku/main.h>

#ifdef __cplusplus
extern "C" {
#endif

void print(CSTRING, ...);
const char * init_kotuku(int argc, CSTRING *argv);
void close_kotuku(void);

#ifdef __cplusplus
}
#endif
