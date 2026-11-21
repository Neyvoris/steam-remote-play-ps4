#pragma once
#include <stddef.h>
int ps4_network_init(void);
int ps4_random_init(void);
int ps4_random(void *data, size_t size);
int ps4_threading_check(void);
