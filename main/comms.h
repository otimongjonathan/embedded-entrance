#pragma once
#include <stdint.h>
#include <stdbool.h>
void comms_init(void);
void comms_start(void);
void comms_publish_occupancy(int count);
void comms_publish_event(uint32_t tag, bool is_in, const char *result);
void comms_publish_schedule(int start, int end);