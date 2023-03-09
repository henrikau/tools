#pragma once

#include <stdbool.h>

bool enable_ftrace(void);
bool start_ftrace(void);
void tag_ftrace(const char *msg);
void stop_ftrace(void);
