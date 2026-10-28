#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

void storage_init(void);

void storage_load_schedule(int *start, int *end);
void storage_save_schedule(int start, int end);

void storage_load_inside(uint32_t *tags, int *n);
void storage_save_inside(const uint32_t *tags, int n);

void storage_log_event(time_t ts, uint32_t tag, bool is_in, const char *result);