#ifndef CHRYSOPOLIS_RUNTIME_FS_H
#define CHRYSOPOLIS_RUNTIME_FS_H 1

#include "runtime_lifecycle.h"

#include <stdbool.h>

void runtime_fs_bind(void);
[[nodiscard]] runtime_status_t runtime_fs_start(bool warm_start);

#endif
