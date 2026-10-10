#include "game_client.h"
#include "lvgl.h"
#if __has_include("client_config.h")
#include "client_config.h"
#else
#error "Copy src/client_config.example.h to src/client_config.h and configure Wi-Fi and the LAN server URL"
#endif
#include <inttypes.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_websocket_client.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define MAX_MESSAGE 4096
#define RX_CHUNK 1024
#define IP_READY BIT0
#define WIFI_LOST BIT1
#define RX_OVERFLOW BIT2
#define RESPONSE_MS 5000

static const char *TAG = "game";
/* Shared model is protected briefly; no network operation holds this mutex.
 * Only the LVGL timer and click callbacks touch widgets. */
static struct {
    char board[9];
    bool winning[9], can_move, connected, synced, pending, syncing, error_wait;
    bool dirty;
    uint64_t revision, pending_revision;
    int pending_cell;
    int64_t deadline;
    char status[48], local[192];
} model;
static SemaphoreHandle_t model_lock;
static EventGroupHandle_t flags;
static QueueHandle_t rx_queue, command_queue;
static lv_obj_t *cells[9], *marks[9], *status_label, *local_label, *reset_button;
/* LVGL 8 dereferences transition descriptors during state changes.
 * An empty property list disables local transitions without a NULL pointer. */
static const lv_style_prop_t no_transition_props[] = {0};
static const lv_style_transition_dsc_t no_transition = {
    .props = no_transition_props,
};

typedef struct { int cell; uint64_t revision; } command_t; /* -1 = reset */
typedef struct {
    int event, len, total, offset;
    uint8_t opcode;
    bool fin;
    char data[RX_CHUNK];
} rx_event_t;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static void lock(void) { xSemaphoreTake(model_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(model_lock); }
static void local_locked(const char *text)
{
    if (strcmp(model.local, text)) {
        snprintf(model.local, sizeof(model.local), "%s", text);
        model.dirty = true;
        if (strcmp(text, "Connected") && strcmp(text, "Waiting for server"))
            ESP_LOGI(TAG, "%s", text);
    }
}
static void offline(const char *reason)
{
    lock();
    model.connected = model.synced = model.pending = model.syncing = false;
    model.error_wait = false;
    model.dirty = true;
    if (reason) local_locked(reason);
    /* Dropping commands while holding the model lock excludes click enqueue. */
    xQueueReset(command_queue);
    unlock();
}

static void enabled(lv_obj_t *obj, bool yes)
{
    if (yes) lv_obj_clear_state(obj, LV_STATE_DISABLED);
    else lv_obj_add_state(obj, LV_STATE_DISABLED);
}
static void render_locked(void)
{
    bool ready = model.connected && model.synced && !model.pending && !model.syncing;
    for (int i = 0; i < 9; ++i) {
        char mark[2] = {model.board[i], 0};
        lv_label_set_text(marks[i], mark);
        enabled(cells[i], ready && model.can_move && !model.board[i]);
        lv_color_t color = lv_color_hex(model.winning[i] ? 0x267443 : 0x253a50);
        lv_obj_set_style_bg_color(cells[i], color, LV_PART_MAIN);
        lv_obj_set_style_bg_color(cells[i], color, LV_PART_MAIN | LV_STATE_DISABLED);
    }
    enabled(reset_button, ready);
    lv_label_set_text(status_label, model.status);
    lv_label_set_text(local_label, model.local);
    model.dirty = false;
}
static void ui_tick(lv_timer_t *timer)
{
    (void)timer;
    if (xSemaphoreTake(model_lock, 0) == pdTRUE) {
        if (model.dirty) render_locked();
        unlock();
    }
}
static void clicked(lv_event_t *event)
{
    int cell = (int)(intptr_t)lv_event_get_user_data(event);
    if (xSemaphoreTake(model_lock, 0) != pdTRUE) return;
    if (model.connected && model.synced && !model.pending && !model.syncing &&
        (cell < 0 || (model.can_move && !model.board[cell]))) {
        command_t cmd = {.cell = cell, .revision = model.revision};
        if (xQueueSend(command_queue, &cmd, 0) == pdTRUE) {
            model.pending = true;
            model.pending_revision = model.revision;
            model.pending_cell = cell;
            model.deadline = now_ms() + RESPONSE_MS;
            model.error_wait = false;
            local_locked("Waiting for server");
            render_locked();
        }
    }
    unlock();
}
static void create_ui(void)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101923), 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    /* Landscape layout: board on the left, controls on the right. */
    for (int i = 0; i < 9; ++i) {
        cells[i] = lv_btn_create(screen);
        lv_obj_set_pos(cells[i], 8 + (i % 3) * 98, 16 + (i / 3) * 98);
        lv_obj_set_size(cells[i], 92, 92);
        lv_obj_set_style_radius(cells[i], 6, 0);
        lv_obj_set_style_shadow_width(cells[i], 0, 0);
        lv_obj_set_style_transition(cells[i], &no_transition, 0);
        lv_obj_set_style_transition(cells[i], &no_transition, LV_STATE_PRESSED);
        lv_obj_set_style_opa(cells[i], LV_OPA_COVER, LV_STATE_DISABLED);
        lv_obj_set_style_text_color(cells[i], lv_color_white(), LV_STATE_DISABLED);
        lv_obj_add_event_cb(cells[i], clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        marks[i] = lv_label_create(cells[i]);
        lv_obj_set_style_text_font(marks[i], &lv_font_montserrat_48, 0);
        lv_obj_center(marks[i]);
    }
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Tic-tac-toe\nYou: X   CPU: O");
    lv_obj_set_pos(title, 310, 16);
    status_label = lv_label_create(screen);
    lv_obj_set_pos(status_label, 310, 72);
    lv_obj_set_width(status_label, 162);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_20, 0);
    reset_button = lv_btn_create(screen);
    lv_obj_set_pos(reset_button, 310, 140);
    lv_obj_set_size(reset_button, 162, 44);
    lv_obj_set_style_transition(reset_button, &no_transition, 0);
    lv_obj_set_style_transition(reset_button, &no_transition, LV_STATE_PRESSED);
    lv_obj_add_event_cb(reset_button, clicked, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    lv_obj_t *label = lv_label_create(reset_button);
    lv_label_set_text(label, "New Game");
    lv_obj_center(label);
    local_label = lv_label_create(screen);
    lv_obj_set_pos(local_label, 310, 200);
    lv_obj_set_size(local_label, 162, 112);
    lv_label_set_long_mode(local_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(local_label, &lv_font_montserrat_14, 0);
    render_locked();
    lv_timer_create(ui_tick, 30, NULL);
}

static const cJSON *field(const cJSON *json, const char *name)
{
    return cJSON_GetObjectItemCaseSensitive(json, name);
}
static bool string_is(const cJSON *item, const char *text)
{
    return cJSON_IsString(item) && !strcmp(item->valuestring, text);
}
static bool integer(const cJSON *item, double max)
{
    return cJSON_IsNumber(item) && isfinite(item->valuedouble) &&
           item->valuedouble >= 0 && item->valuedouble <= max &&
           floor(item->valuedouble) == item->valuedouble;
}
/* Validate the UI contract, not game rules; the server decides all outcomes. */
static bool apply_state(const cJSON *json)
{
    const cJSON *board = field(json, "board"), *win = field(json, "winningCells");
    const cJSON *rev = field(json, "revision"), *turn = field(json, "turn");
    const cJSON *result = field(json, "result"), *status = field(json, "statusText");
    const cJSON *can_move = field(json, "canMove");
    char next_board[9] = {0};
    bool next_win[9] = {0};
    if (!integer(rev, 9007199254740991.0) || !cJSON_IsArray(board) ||
        cJSON_GetArraySize(board) != 9 || !cJSON_IsArray(win) ||
        !cJSON_IsBool(can_move) || !cJSON_IsString(status) ||
        strlen(status->valuestring) >= sizeof(model.status)) return false;
    bool playing = string_is(result, "playing");
    bool won = string_is(result, "X_won") || string_is(result, "O_won");
    if (!playing && !won && !string_is(result, "draw")) return false;
    if (playing ? !(string_is(turn, "X") || string_is(turn, "O")) : !cJSON_IsNull(turn)) return false;
    if (cJSON_IsTrue(can_move) != (playing && string_is(turn, "X"))) return false;
    int count = cJSON_GetArraySize(win);
    if (count != (won ? 3 : 0)) return false;
    for (int i = 0; i < 9; ++i) {
        const cJSON *mark = cJSON_GetArrayItem(board, i);
        if (!string_is(mark, "") && !string_is(mark, "X") && !string_is(mark, "O")) return false;
        next_board[i] = mark->valuestring[0];
    }
    for (int i = 0; i < count; ++i) {
        const cJSON *cell = cJSON_GetArrayItem(win, i);
        if (!integer(cell, 8) || next_win[cell->valueint]) return false;
        next_win[cell->valueint] = true;
    }
    uint64_t revision = (uint64_t)rev->valuedouble;
    lock();
    if (model.synced && revision < model.revision) { unlock(); return true; }
    bool first = !model.synced;
    bool command_state = false;
    if (model.pending && revision > model.pending_revision) {
        if (model.pending_cell >= 0) command_state = next_board[model.pending_cell] == 'X';
        else {
            command_state = playing && string_is(turn, "X");
            for (int i = 0; i < 9; ++i) if (next_board[i]) command_state = false;
        }
    }
    memcpy(model.board, next_board, sizeof(next_board));
    memcpy(model.winning, next_win, sizeof(next_win));
    snprintf(model.status, sizeof(model.status), "%s", status->valuestring);
    model.can_move = cJSON_IsTrue(can_move);
    model.revision = revision;
    model.synced = true;
    /* Equal revisions acknowledge sync/errors, not an unrelated broadcast
     * (e.g. a monitor joining while a command is in flight). */
    if (first || model.syncing || model.error_wait ||
        command_state) {
        model.pending = model.syncing = false;
        if (!model.error_wait) local_locked("Connected");
        model.error_wait = false;
    }
    model.dirty = true;
    unlock();
    return true;
}

/* Callbacks copy bounded chunks only. They never call LVGL, stop or send. */
static void websocket_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id != WEBSOCKET_EVENT_CONNECTED && id != WEBSOCKET_EVENT_DISCONNECTED &&
        id != WEBSOCKET_EVENT_CLOSED && id != WEBSOCKET_EVENT_ERROR &&
        id != WEBSOCKET_EVENT_DATA) return;
    rx_event_t event = {.event = id};
    if (id == WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *d = data;
        if (d->data_len < 0 || d->data_len > RX_CHUNK) {
            xEventGroupSetBits(flags, RX_OVERFLOW);
            return;
        }
        event.len = d->data_len; event.total = d->payload_len;
        event.offset = d->payload_offset; event.opcode = d->op_code; event.fin = d->fin;
        if (event.len) memcpy(event.data, d->data_ptr, event.len);
    }
    if (xQueueSend(rx_queue, &event, 0) != pdTRUE) xEventGroupSetBits(flags, RX_OVERFLOW);
}
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(flags, IP_READY);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "Wi-Fi disconnect reason=%u", (unsigned)event->reason);
        xEventGroupClearBits(flags, IP_READY);
        xEventGroupSetBits(flags, WIFI_LOST);
    }
}

static bool send_text(esp_websocket_client_handle_t ws, const char *text)
{
    int length = strlen(text);
    return esp_websocket_client_is_connected(ws) &&
        esp_websocket_client_send_text(ws, text, length, pdMS_TO_TICKS(1000)) == length;
}
static unsigned backoff(unsigned *step)
{
    static const unsigned waits[] = {1000, 2000, 4000, 8000, 10000};
    unsigned wait = waits[*step];
    if (*step < 4) ++*step;
    return wait;
}

typedef struct {
    char text[MAX_MESSAGE + 1];
    size_t used;
    int frame_offset, frame_length;
    bool active;
} assembly_t;

/* Also handles a frame split into library buffers and RFC6455 continuation
 * frames. Ping/pong may be interleaved. Reject rather than truncate. */
static int assemble(assembly_t *a, const rx_event_t *e)
{
    if (e->opcode == 9 || e->opcode == 10) return 0;
    if (e->opcode == 8) return -1;
    if (e->opcode != 1 && e->opcode != 0) return -1;
    if (e->offset < 0 || e->total < 0 || e->total > MAX_MESSAGE ||
        e->len > e->total || e->offset > e->total - e->len) return -1;
    if (e->offset == 0) {
        if (a->frame_offset != 0) return -1;
        if (e->opcode == 1) {
            if (a->active) return -1;
            a->active = true;
            a->used = 0;
        } else if (!a->active) return -1;
        a->frame_length = e->total;
    }
    if (!a->active || e->offset != a->frame_offset ||
        e->total != a->frame_length || a->used + e->len > MAX_MESSAGE) return -1;
    memcpy(a->text + a->used, e->data, e->len);
    a->used += e->len;
    a->frame_offset += e->len;
    if (a->frame_offset != a->frame_length) return 0;
    a->frame_offset = 0;
    if (!e->fin) return 0;
    a->active = false;
    a->text[a->used] = 0;
    return 1;
}

/* Return false to reconnect on malformed data or controller rejection. */
static bool message_received(assembly_t *a, bool *busy)
{
    /* Bound cJSON recursion as well as bytes. Reject embedded NUL strings so
     * C string comparison cannot accept a symbol such as X\\u0000garbage. */
    if (memchr(a->text, 0, a->used) || strstr(a->text, "\\u0000")) return false;
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < a->used; ++i) {
        char c = a->text[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > 16) return false; }
        else if (c == '}' || c == ']') { if (!depth) return false; --depth; }
    }
    if (depth || quoted) return false;
    const char *end = NULL;
    cJSON *json = cJSON_ParseWithLengthOpts(a->text, a->used + 1, &end, true);
    bool valid = json && end == a->text + a->used && cJSON_IsObject(json) &&
                 integer(field(json, "v"), 1) && field(json, "v")->valueint == 1 &&
                 cJSON_IsString(field(json, "type"));
    if (valid && string_is(field(json, "type"), "state")) valid = apply_state(json);
    else if (valid && string_is(field(json, "type"), "error")) {
        const cJSON *code = field(json, "code"), *msg = field(json, "message");
        valid = cJSON_IsString(code) && cJSON_IsString(msg) &&
                strlen(code->valuestring) < 48 && strlen(msg->valuestring) < 128;
        if (valid) {
            char text[192];
            snprintf(text, sizeof(text), "%s: %s", code->valuestring, msg->valuestring);
            lock();
            local_locked(text);
            model.error_wait = true;
            model.pending = true;
            model.deadline = now_ms() + RESPONSE_MS;
            model.dirty = true;
            unlock();
            *busy = string_is(code, "CONTROLLER_BUSY");
        }
    }
    /* Unknown version-1 message types have no effect. */
    cJSON_Delete(json);
    return valid && !*busy;
}

static void network_task(void *arg)
{
    (void)arg;
    /* Keep library logs quiet (no URI/credential dumps); app logs transitions. */
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("websocket_client", ESP_LOG_NONE);
    esp_log_level_set("transport_ws", ESP_LOG_NONE);
    const char *failed_step = "nvs_flash_init";
    esp_err_t err = nvs_flash_init();
    /* Network startup failure must not abort the display/UI task. */
#define NETWORK_START(call) do { \
    failed_step = #call; \
    err = (call); \
    if (err != ESP_OK) goto network_failed; \
} while (0)
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        NETWORK_START(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        failed_step = "nvs_flash_init";
        goto network_failed;
    }
    NETWORK_START(esp_netif_init());
    NETWORK_START(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    NETWORK_START(netif ? ESP_OK : ESP_ERR_NO_MEM);
    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    NETWORK_START(esp_wifi_init(&wifi_init));
    NETWORK_START(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    NETWORK_START(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    wifi_config_t wifi = {0};
    _Static_assert(sizeof(CLIENT_WIFI_SSID) - 1 <= 32, "SSID is too long");
    _Static_assert(sizeof(CLIENT_WIFI_PASSWORD) - 1 <= 64, "Wi-Fi password is too long");
    memcpy(wifi.sta.ssid, CLIENT_WIFI_SSID, sizeof(CLIENT_WIFI_SSID) - 1);
    memcpy(wifi.sta.password, CLIENT_WIFI_PASSWORD, sizeof(CLIENT_WIFI_PASSWORD) - 1);
    NETWORK_START(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    NETWORK_START(esp_wifi_set_mode(WIFI_MODE_STA));
    NETWORK_START(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    NETWORK_START(esp_wifi_start());
#undef NETWORK_START

    esp_websocket_client_handle_t ws = NULL;
    unsigned wifi_step = 0, ws_step = 0;
    int64_t wifi_due = 0, ws_due = 0, wifi_deadline = 0, connect_deadline = 0;
    bool wifi_attempt = false, ws_connected = false, busy = false;
    /* Heap/static storage avoids large RX/JSON buffers on the LVGL stack. */
    static assembly_t assembly;
    static rx_event_t event;
    for (;;) {
        int64_t now = now_ms();
        EventBits_t bits = xEventGroupGetBits(flags);
        bool reconnect = false;
        const char *reason = NULL;
        if (bits & WIFI_LOST) {
            xEventGroupClearBits(flags, WIFI_LOST);
            wifi_attempt = false;
            wifi_due = now + backoff(&wifi_step);
            reconnect = true; reason = "Wi-Fi disconnected; retrying";
        }
        if (!(bits & IP_READY)) {
            if (ws) { reconnect = true; reason = "Wi-Fi disconnected; retrying"; }
            if (wifi_attempt && now >= wifi_deadline) {
                esp_wifi_disconnect();
                wifi_attempt = false;
                wifi_due = now + backoff(&wifi_step);
                offline("Wi-Fi timeout; check configuration");
            }
            if (!wifi_attempt && now >= wifi_due) {
                offline("Connecting Wi-Fi");
                if (esp_wifi_connect() == ESP_OK) {
                    wifi_attempt = true; wifi_deadline = now + 15000;
                } else wifi_due = now + backoff(&wifi_step);
            }
        } else {
            wifi_attempt = false; wifi_step = 0;
        }
        if (bits & RX_OVERFLOW) {
            xEventGroupClearBits(flags, RX_OVERFLOW);
            reconnect = true; reason = "Receive overflow; reconnecting";
        }
        while (!reconnect && xQueueReceive(rx_queue, &event, 0) == pdTRUE) {
            if (event.event == WEBSOCKET_EVENT_CONNECTED) {
                ws_connected = true;
                lock();
                model.connected = true; model.synced = false;
                model.deadline = now_ms() + RESPONSE_MS;
                local_locked("Connected; waiting for state");
                model.dirty = true;
                unlock();
                if (!send_text(ws, "{\"v\":1,\"type\":\"hello\",\"role\":\"esp\"}")) {
                    reconnect = true; reason = "Hello failed; reconnecting";
                }
            } else if (event.event == WEBSOCKET_EVENT_DATA) {
                int result = assemble(&assembly, &event);
                if (result < 0 || (result > 0 && !message_received(&assembly, &busy))) {
                    reconnect = true;
                    reason = busy ? NULL : "Invalid message or socket closed; reconnecting";
                } else if (result > 0) {
                    lock();
                    if (model.synced) ws_step = 0;
                    unlock();
                }
            } else {
                reconnect = true;
                reason = busy ? NULL : "Server disconnected; retrying";
            }
        }
        if (ws && !ws_connected && now >= connect_deadline) {
            reconnect = true; reason = "Server connection timeout; retrying";
        }
        if (ws_connected && !reconnect) {
            command_t cmd;
            if (xQueueReceive(command_queue, &cmd, 0) == pdTRUE) {
                char text[112];
                if (cmd.cell < 0) snprintf(text, sizeof(text),
                    "{\"v\":1,\"type\":\"reset\",\"revision\":%" PRIu64 "}", cmd.revision);
                else snprintf(text, sizeof(text),
                    "{\"v\":1,\"type\":\"move\",\"cell\":%d,\"revision\":%" PRIu64 "}", cmd.cell, cmd.revision);
                if (!send_text(ws, text)) { reconnect = true; reason = "Send failed; reconnecting"; }
            }
            bool sync = false;
            lock();
            if ((!model.synced || model.pending || model.syncing) && now_ms() >= model.deadline) {
                if (model.syncing) { reconnect = true; reason = "Sync timed out; reconnecting"; }
                else {
                    model.syncing = true; model.deadline = now_ms() + RESPONSE_MS;
                    model.dirty = true;
                    local_locked("Response timeout; synchronizing");
                    sync = true;
                }
            }
            unlock();
            if (sync && !reconnect && !send_text(ws, "{\"v\":1,\"type\":\"sync\"}")) {
                reconnect = true; reason = "Sync send failed; reconnecting";
            }
        }
        if (reconnect) {
            offline(reason);
            if (ws) {
                /* Outside both event callbacks and the LVGL task/lock. */
                esp_websocket_client_stop(ws);
                esp_websocket_client_destroy(ws);
                ws = NULL;
            }
            ws_connected = false;
            memset(&assembly, 0, sizeof(assembly));
            xQueueReset(rx_queue);
            xEventGroupClearBits(flags, RX_OVERFLOW);
            ws_due = now_ms() + backoff(&ws_step);
        }
        if (!ws && (xEventGroupGetBits(flags) & IP_READY) && now_ms() >= ws_due) {
            /* Keep CONTROLLER_BUSY visible throughout the retry attempt. */
            if (!busy) offline("Connecting to server");
            busy = false;
            esp_websocket_client_config_t config = {
                .uri = CLIENT_SERVER_URL,
                .disable_auto_reconnect = true,
                .buffer_size = RX_CHUNK,
                .task_stack = 6144,
                .network_timeout_ms = 3000,
                .ping_interval_sec = 10,
                .pingpong_timeout_sec = 10,
            };
            ws = esp_websocket_client_init(&config);
            if (!ws || esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, websocket_event, NULL) != ESP_OK ||
                esp_websocket_client_start(ws) != ESP_OK) {
                if (ws) esp_websocket_client_destroy(ws);
                ws = NULL;
                offline("WebSocket startup failed; retrying");
                ws_due = now_ms() + backoff(&ws_step);
            } else connect_deadline = now_ms() + 10000;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

network_failed:
    ESP_LOGE(TAG, "Network startup failed at %s: %s", failed_step, esp_err_to_name(err));
    char error_text[96];
    snprintf(error_text, sizeof(error_text), "Network startup failed: %s", esp_err_to_name(err));
    offline(error_text);
    vTaskDelete(NULL);
}

void game_client_init(void)
{
    model_lock = xSemaphoreCreateMutex();
    flags = xEventGroupCreate();
    rx_queue = xQueueCreate(8, sizeof(rx_event_t));
    command_queue = xQueueCreate(1, sizeof(command_t));
    assert(model_lock && flags && rx_queue && command_queue);
    snprintf(model.status, sizeof(model.status), "Waiting for game");
    snprintf(model.local, sizeof(model.local), "Starting network");
    create_ui();
    BaseType_t ok = xTaskCreate(network_task, "game network", 8192, NULL, 3, NULL);
    if (ok != pdPASS) offline("Network task allocation failed");
}
