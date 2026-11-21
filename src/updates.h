#pragma once
#include <stddef.h>
#include <stdbool.h>

void updates_check(char *message, size_t capacity);
bool updates_install(char *message, size_t capacity);
