#include "state.h"
#include "config.h"
#include "storage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

typedef struct {
    SemaphoreHandle_t mtx;
    int      occupancy;
    uint32_t inside[INSIDE_MAX];
    int      inside_n;
    int      sched_start;
    int      sched_end;
    bool     admin;
    time_t   admin_since;
    room_state_t room[2];
} state_data_t;

static state_data_t s;

const char *state_result_str(result_t r) {
    switch (r) {
        case RES_OK:                 return "OK";
        case RES_OK_AFTER_HOURS:     return "OK_AFTER_HOURS";
        case RES_DENIED_UNKNOWN:     return "DENIED_UNKNOWN";
        case RES_DENIED_SCHEDULE:    return "DENIED_SCHEDULE";
        case RES_DENIED_ALREADY_IN:  return "DENIED_ALREADY_IN";
        case RES_DENIED_NOT_IN:      return "DENIED_NOT_IN";
    }
    return "?";
}

static bool in_window(int mod) {
    if (s.sched_start == s.sched_end) return true;
    return (s.sched_start <= s.sched_end)
         ? (mod >= s.sched_start && mod < s.sched_end)
         : (mod >= s.sched_start || mod < s.sched_end);
}

static int find_inside(uint32_t tag) {
    for (int i = 0; i < s.inside_n; i++) if (s.inside[i] == tag) return i;
    return -1;
}

void state_init(void) {
    memset(&s, 0, sizeof(s));
    s.mtx = xSemaphoreCreateMutex();
    s.sched_start = 0;
    s.sched_end   = 0;

    int st = -1, en = -1;
    storage_load_schedule(&st, &en);
    if (st >= 0 && en >= 0) { s.sched_start = st; s.sched_end = en; }

    uint32_t tags[INSIDE_MAX]; int n = 0;
    storage_load_inside(tags, &n);
    if (n > INSIDE_MAX) n = INSIDE_MAX;
    memcpy(s.inside, tags, n * sizeof(uint32_t));
    s.inside_n  = n;
    s.occupancy = n;
}

void state_persist(void) {
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    int st = s.sched_start, en = s.sched_end;
    int n  = s.inside_n;
    uint32_t tags[INSIDE_MAX];
    memcpy(tags, s.inside, n * sizeof(uint32_t));
    xSemaphoreGive(s.mtx);
    storage_save_schedule(st, en);
    storage_save_inside(tags, n);
}

int  state_occupancy(void) {
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    int v = s.occupancy;
    xSemaphoreGive(s.mtx);
    return v;
}

int  state_inside_count(void) {
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    int v = s.inside_n;
    xSemaphoreGive(s.mtx);
    return v;
}

uint32_t state_inside_at(int idx) {
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    uint32_t v = (idx >= 0 && idx < s.inside_n) ? s.inside[idx] : 0;
    xSemaphoreGive(s.mtx);
    return v;
}

void state_get_schedule(int *start, int *end) {
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    *start = s.sched_start; *end = s.sched_end;
    xSemaphoreGive(s.mtx);
}

void state_set_schedule(int start, int end) {
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    s.sched_start = start; s.sched_end = end;
    xSemaphoreGive(s.mtx);
}

result_t state_evaluate(uint32_t tag, bool is_entry, time_t now) {
    struct tm tm; localtime_r(&now, &tm);
    int mod = tm.tm_hour * 60 + tm.tm_min;
    bool known = false;
    for (size_t i = 0; i < N_AUTH; i++)
        if (AUTHORIZED_TAGS[i] == tag) { known = true; break; }

    xSemaphoreTake(s.mtx, portMAX_DELAY);
    result_t res;
    if (!known) {
        res = RES_DENIED_UNKNOWN;
    } else if (is_entry) {
        if (!in_window(mod))            res = RES_DENIED_SCHEDULE;
        else if (find_inside(tag) >= 0) res = RES_DENIED_ALREADY_IN;
        else {
            s.inside[s.inside_n++] = tag;
            s.occupancy = s.inside_n;
            res = RES_OK;
        }
    } else {
        int idx = find_inside(tag);
        if (idx < 0) res = RES_DENIED_NOT_IN;
        else {
            for (int i = idx; i < s.inside_n - 1; i++)
                s.inside[i] = s.inside[i+1];
            s.inside_n--;
            s.occupancy = s.inside_n;
            res = in_window(mod) ? RES_OK : RES_OK_AFTER_HOURS;
        }
    }
    xSemaphoreGive(s.mtx);
    return res;
}

bool state_admin_active(void) {
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    bool ok = s.admin && (time(NULL) - s.admin_since < ADMIN_TTL_S);
    if (!ok) s.admin = false;
    xSemaphoreGive(s.mtx);
    return ok;
}

bool state_admin_login(const char *pin) {
    if (strcmp(pin, "1234") != 0) return false; 
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    s.admin = true; s.admin_since = time(NULL);
    xSemaphoreGive(s.mtx);
    return true;
}

void state_admin_logout(void) {
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    s.admin = false;
    xSemaphoreGive(s.mtx);
}

void state_room_get(int room, room_state_t *out) {
    if (room < 0 || room > 1) return;
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    *out = s.room[room];
    xSemaphoreGive(s.mtx);
}

void state_room_set(int room, bool occupied, bool lights) {
    if (room < 0 || room > 1) return;
    xSemaphoreTake(s.mtx, portMAX_DELAY);
    s.room[room].occupied = occupied;
    s.room[room].lights   = lights;
    xSemaphoreGive(s.mtx);
}