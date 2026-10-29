#include "app_state.h"
#include "ui_limits.h"
#define DISPLAY_MS UI_DISPLAY_MS
#define DOUBLE_CLICK_MS 400
#define RATING_IDLE_MS 5000
#define FEEDBACK_MS 1000

void app_state_shown(app_state_t *state, uint32_t item_id, int64_t now_ms)
{
    *state = (app_state_t){.view = APP_NORMAL, .item_id = item_id,
        .display_deadline = now_ms + DISPLAY_MS, .remaining_ms = DISPLAY_MS};
}

static app_action_t request_next(app_state_t *state)
{
    state->click_pending = false;
    if (state->next_requested) return APP_ACTION_NONE;
    state->next_requested = true;
    return APP_ACTION_NEXT;
}

app_action_t app_state_button(app_state_t *state, int64_t timestamp_ms)
{
    if (state->view == APP_NORMAL) {
        if (!state->click_pending) {
            state->click_pending = true;
            state->first_click = timestamp_ms;
        } else if (timestamp_ms >= state->first_click && timestamp_ms - state->first_click < DOUBLE_CLICK_MS) {
            state->click_pending = false;
            state->view = APP_RATING;
            state->rating = 0;
            state->remaining_ms = state->display_deadline > timestamp_ms ? state->display_deadline - timestamp_ms : 0;
            state->rating_deadline = timestamp_ms + RATING_IDLE_MS;
            state->last_rating_click = timestamp_ms;
            return APP_ACTION_RATING;
        } else return request_next(state);
    } else if (state->view == APP_RATING && timestamp_ms >= state->last_rating_click + 200) {
        state->last_rating_click = timestamp_ms;
        state->rating_deadline = timestamp_ms + RATING_IDLE_MS;
        state->rating = (state->rating + 1) % 6;
        return APP_ACTION_RATING;
    }
    return APP_ACTION_NONE;
}

app_action_t app_state_tick(app_state_t *state, int64_t now_ms)
{
    if (state->view == APP_NORMAL) {
        if ((state->click_pending && now_ms >= state->first_click + DOUBLE_CLICK_MS)
            || (!state->click_pending && now_ms >= state->display_deadline)) return request_next(state);
    } else if (state->view == APP_RATING && now_ms >= state->rating_deadline) {
        if (!state->rating) {
            state->view = APP_NORMAL;
            state->display_deadline = now_ms + state->remaining_ms;
            return APP_ACTION_RESTORE;
        }
        state->view = APP_SUBMITTING;
        return APP_ACTION_SUBMIT;
    } else if (state->view == APP_FEEDBACK && now_ms >= state->feedback_deadline) {
        state->view = APP_NORMAL;
        state->display_deadline = now_ms + state->remaining_ms;
        return APP_ACTION_RESTORE;
    }
    return APP_ACTION_NONE;
}

void app_state_rating_done(app_state_t *state, int64_t now_ms)
{
    if (state->view == APP_SUBMITTING) {
        state->view = APP_FEEDBACK;
        state->feedback_deadline = now_ms + FEEDBACK_MS;
    }
}

int app_state_progress(const app_state_t *state, int64_t now_ms)
{
    int64_t remaining = state->view == APP_NORMAL ? state->display_deadline - now_ms : state->remaining_ms;
    if (remaining <= 0) return 100;
    if (remaining >= DISPLAY_MS) return 0;
    return (int)((DISPLAY_MS - remaining) * 100 / DISPLAY_MS);
}
