#pragma once
#include <stdbool.h>

void startup_log_open(void);
bool startup_log_available(void);
void startup_log_step(const char *name);
void startup_log_result(const char *name, int result);
void startup_log_close(void);
