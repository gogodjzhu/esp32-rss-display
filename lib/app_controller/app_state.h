#ifndef APP_STATE_H
#define APP_STATE_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { APP_EMPTY, APP_NORMAL, APP_RATING, APP_SUBMITTING, APP_FEEDBACK } app_view_state_t;
typedef enum { APP_ACTION_NONE, APP_ACTION_NEXT, APP_ACTION_RATING, APP_ACTION_SUBMIT, APP_ACTION_RESTORE } app_action_t;
typedef struct {
    app_view_state_t view;
    uint32_t item_id;
    int rating;
    int64_t display_deadline, remaining_ms, rating_deadline, feedback_deadline;
    int64_t first_click, last_rating_click;
    bool click_pending, next_requested;
} app_state_t;
void app_state_shown(app_state_t *state, uint32_t item_id, int64_t now_ms);
app_action_t app_state_button(app_state_t *state, int64_t timestamp_ms);
app_action_t app_state_tick(app_state_t *state, int64_t now_ms);
void app_state_rating_done(app_state_t *state, int64_t now_ms);
int app_state_progress(const app_state_t *state, int64_t now_ms);
#ifdef __cplusplus
}
#endif
#endif
