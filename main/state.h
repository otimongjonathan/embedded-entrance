#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef enum {
    RES_OK,
    RES_OK_AFTER_HOURS,
    RES_DENIED_UNKNOWN,
    RES_DENIED_SCHEDULE,
    RES_DENIED_ALREADY_IN,
    RES_DENIED_NOT_IN
} result_t;

const char *state_result_str(result_t r);

void state_init(void);
void state_persist(void);

int  state_occupancy(void);
int  state_inside_count(void);
uint32_t state_inside_at(int idx);

void state_get_schedule(int *start, int *end);
void state_set_schedule(int start, int end);

result_t state_evaluate(uint32_t tag, bool is_entry, time_t now);

bool state_admin_active(void);
bool state_admin_login(const char *pin);
void state_admin_logout(void);

typedef struct { bool occupied; bool lights; } room_state_t;
void state_room_get(int room, room_state_t *out);
void state_room_set(int room, bool occupied, bool lights);