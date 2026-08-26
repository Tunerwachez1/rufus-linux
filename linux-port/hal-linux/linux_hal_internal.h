/*
 * Rufus Linux port - internal interface shared by the Linux HAL files
 * GPLv3
 */
#ifndef RUFUS_LINUX_HAL_INTERNAL_H
#define RUFUS_LINUX_HAL_INTERNAL_H

#include "hal.h"

int rfd_linux_enumerate_devices(rfd_device *out, int max_count);
int rfd_linux_list_partitions(const char *node, dev_t *out, int max_count);

#endif
