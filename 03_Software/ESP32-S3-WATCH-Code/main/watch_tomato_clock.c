/**
 * @file watch_tomato_clock.c
 * @brief 秒表页面与正计时状态机。
 *
 * 本文件原为番茄钟实现，现改造为秒表：
 * - 固定正计时，从 00:00:00 开始累加；
 * - 不再需要时间预设、方向切换、字段滚轮编辑；
 * - 交互简化为 返回 / 启动暂停 / 重置 三个焦点。
 * 对外接口名保留 watch_tomato_clock_* 不变，避免上层改动。
 */


#include "watch_tomato_clock.h"
#include "watch_language.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

LV_FONT_DECLARE(cn_font_26);

/* 以下宏大多是 UI 坐标、尺寸或任务参数。
 * 修改这类值时建议同时检查：
 * 1. 240x240 屏幕边界是否越界；
 * 2. 选择框/动画目标是否仍然对齐；
 * 3. FreeRTOS 任务栈是否足够容纳 LVGL 临时对象。
 */
#define WATCH_SCREEN_W              240
#define WATCH_SCREEN_H              240

#define STOPWATCH_DISPLAY_Y         102

#define STOPWATCH_SELECTOR_PAD_X       7
#define STOPWATCH_SELECTOR_PAD_Y       4
#define STOPWATCH_SELECTOR_RADIUS      8
#define STOPWATCH_SELECTOR_BORDER_W    2
#define STOPWATCH_SELECTOR_ANIM_MS     180

#define STOPWATCH_COUNTDOWN_FRAME_W        220
#define STOPWATCH_COUNTDOWN_FRAME_H        146
#define STOPWATCH_COUNTDOWN_FRAME_RADIUS   12
#define STOPWATCH_COUNTDOWN_FRAME_BORDER_W 5

#define STOPWATCH_COUNTDOWN_FRAME_X        ((WATCH_SCREEN_W - STOPWATCH_COUNTDOWN_FRAME_W) / 2)
#define STOPWATCH_COUNTDOWN_FRAME_Y        50

#define STOPWATCH_TOP_Y                    8

#define STOPWATCH_BOTTOM_PAD               8

#define STOPWATCH_RESET_X                  28
#define STOPWATCH_RESET_Y                  8

#define STOPWATCH_TIMER_PERIOD_MS          1000
#define STOPWATCH_BLINK_HALF_PERIOD_MS     500

/* 秒表上限 99:59:59，到达后自动停止并闪烁提示。 */
#define STOPWATCH_MAX_SECONDS              (99 * 3600 + 59 * 60 + 59)

/**
 * @brief 秒表页面焦点枚举。
 *
 * 焦点在返回、启动/暂停、重置三个按钮间循环。
 */
typedef enum {
    STOPWATCH_FOCUS_BACK = 0,
    STOPWATCH_FOCUS_PLAY_PAUSE,
    STOPWATCH_FOCUS_RESET,
} stopwatch_focus_t;

/**
 * @brief 秒表页面上下文。
 *
 * 集中管理 UI 对象、焦点、计时器运行状态，避免全局变量分散。
 */
typedef struct {
    lv_obj_t *page;
    lv_obj_t *back_btn;

    lv_obj_t *play_label;
    lv_obj_t *reset_bg;
    lv_obj_t *reset_label;

    lv_obj_t *countdown_frame;
    lv_obj_t *countdown_hh;
    lv_obj_t *countdown_colon_1;
    lv_obj_t *countdown_mm;
    lv_obj_t *countdown_colon_2;
    lv_obj_t *countdown_ss;

    lv_obj_t *cursor;
    lv_timer_t *timer;

    stopwatch_focus_t focus;

    bool wants_back;
    bool timer_running;
    bool timer_finished;
    bool reset_pressed;

    bool language_initialized;
    bool language_chinese;

    int elapsed;
} stopwatch_ctx_t;

/* 单例页面状态：该页面同一时间只会存在一个实例。 */
static stopwatch_ctx_t s_sw;

static void stopwatch_cursor_x_anim_cb(void *var, int32_t v)
{
    /* 选择框动画回调：更新 X 坐标。
     */
    lv_obj_set_x((lv_obj_t *)var, v);
}

static void stopwatch_cursor_y_anim_cb(void *var, int32_t v)
{
    /* 选择框动画回调：更新 Y 坐标。
     */
    lv_obj_set_y((lv_obj_t *)var, v);
}

static void stopwatch_cursor_w_anim_cb(void *var, int32_t v)
{
    /* 选择框动画回调：更新宽度。
     */
    lv_obj_set_width((lv_obj_t *)var, v);
}

static void stopwatch_cursor_h_anim_cb(void *var, int32_t v)
{
    /* 选择框动画回调：更新高度。
     */
    lv_obj_set_height((lv_obj_t *)var, v);
}

static void stopwatch_frame_opa_anim_cb(void *var, int32_t v)
{
    /* 到达上限闪烁动画回调，通过透明度变化提示计时已停止。
     */
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static lv_obj_t *stopwatch_obj_from_focus(stopwatch_focus_t focus)
{
    switch(focus) {
    case STOPWATCH_FOCUS_BACK:
        return s_sw.back_btn;

    case STOPWATCH_FOCUS_PLAY_PAUSE:
        return s_sw.play_label;

    case STOPWATCH_FOCUS_RESET:
        return s_sw.reset_label;

    default:
        return NULL;
    }
}

static void stopwatch_total_seconds_to_hms(int total, int *hour, int *min, int *sec)
{
    /* 把总秒数拆回 HH/MM/SS，用于计时过程中刷新显示。
     */
    if(total < 0) {
        total = 0;
    }

    if(total > STOPWATCH_MAX_SECONDS) {
        total = STOPWATCH_MAX_SECONDS;
    }

    *hour = total / 3600;
    total %= 3600;
    *min = total / 60;
    *sec = total % 60;
}

static void stopwatch_label_style_26(lv_obj_t *label)
{
    /* 统一秒表页面 26 号文本样式。
     */
    lv_obj_set_style_text_font(label, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
}

static void stopwatch_cursor_start_anim(lv_obj_t *obj,
                                        lv_anim_exec_xcb_t exec_cb,
                                        int32_t from,
                                        int32_t to)
{
    /* 启动焦点选择框动画，所有坐标和尺寸变化复用此函数。
     */
    lv_anim_t a;

    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, STOPWATCH_SELECTOR_ANIM_MS);
    lv_anim_set_exec_cb(&a, exec_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/**
 * @brief 根据当前焦点更新选择框位置和大小。
 */
static void stopwatch_cursor_update(bool anim)
{
    /* 根据当前焦点更新选择框的位置和大小，保证焦点反馈跟随控件实际尺寸。
     */
    if(s_sw.cursor == NULL || s_sw.back_btn == NULL) {
        return;
    }

    lv_obj_t *target = stopwatch_obj_from_focus(s_sw.focus);

    if(target == NULL) {
        return;
    }

    lv_obj_update_layout(s_sw.page);

    lv_coord_t x = lv_obj_get_x(target) - STOPWATCH_SELECTOR_PAD_X;
    lv_coord_t y = lv_obj_get_y(target) - STOPWATCH_SELECTOR_PAD_Y;
    lv_coord_t w = lv_obj_get_width(target) + STOPWATCH_SELECTOR_PAD_X * 2;
    lv_coord_t h = lv_obj_get_height(target) + STOPWATCH_SELECTOR_PAD_Y * 2;

    if(w < 16 + STOPWATCH_SELECTOR_PAD_X * 2) {
        w = 16 + STOPWATCH_SELECTOR_PAD_X * 2;
    }

    if(!anim || lv_obj_get_width(s_sw.cursor) <= 0 || lv_obj_get_height(s_sw.cursor) <= 0) {
        lv_obj_set_pos(s_sw.cursor, x, y);
        lv_obj_set_size(s_sw.cursor, w, h);
    }
    else {
        stopwatch_cursor_start_anim(s_sw.cursor, stopwatch_cursor_x_anim_cb, lv_obj_get_x(s_sw.cursor), x);
        stopwatch_cursor_start_anim(s_sw.cursor, stopwatch_cursor_y_anim_cb, lv_obj_get_y(s_sw.cursor), y);
        stopwatch_cursor_start_anim(s_sw.cursor, stopwatch_cursor_w_anim_cb, lv_obj_get_width(s_sw.cursor), w);
        stopwatch_cursor_start_anim(s_sw.cursor, stopwatch_cursor_h_anim_cb, lv_obj_get_height(s_sw.cursor), h);
    }

    lv_obj_move_foreground(s_sw.cursor);
}

static void stopwatch_reset_bg_update(void)
{
    /* 刷新重置按钮背景，用于给短暂按下状态提供视觉反馈。
     */
    if(s_sw.reset_bg == NULL || s_sw.reset_label == NULL) {
        return;
    }

    lv_obj_update_layout(s_sw.page);

    lv_coord_t label_x = lv_obj_get_x(s_sw.reset_label);
    lv_coord_t label_y = lv_obj_get_y(s_sw.reset_label);
    lv_coord_t label_w = lv_obj_get_width(s_sw.reset_label);
    lv_coord_t label_h = lv_obj_get_height(s_sw.reset_label);

    lv_obj_set_size(s_sw.reset_bg, label_w + 18, label_h + 8);
    lv_obj_set_pos(s_sw.reset_bg, label_x - 9, label_y - 4);

    if(s_sw.reset_pressed) {
        lv_obj_clear_flag(s_sw.reset_bg, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_sw.reset_bg, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief 根据当前系统语言刷新秒表文本。
 *
 * @param force true 表示强制刷新，false 表示仅在语言变化时刷新。
 */
static void stopwatch_apply_language(bool force)
{
    bool chinese = watch_language_is_chinese();

    if(!force &&
       s_sw.language_initialized &&
       s_sw.language_chinese == chinese) {
        return;
    }

    s_sw.language_initialized = true;
    s_sw.language_chinese = chinese;

    if(s_sw.reset_label != NULL) {
        lv_obj_set_style_text_font(s_sw.reset_label,
                                   chinese ? &cn_font_26 : &lv_font_montserrat_26,
                                   0);
        lv_label_set_text(s_sw.reset_label,
                          chinese ? "清零" : "RST");
        lv_obj_align(s_sw.reset_label,
                     LV_ALIGN_BOTTOM_RIGHT,
                     -STOPWATCH_RESET_X,
                     -STOPWATCH_RESET_Y);
    }

    stopwatch_reset_bg_update();

    if(s_sw.cursor != NULL) {
        stopwatch_cursor_update(false);
    }
}

static void stopwatch_play_label_update(void)
{
    /* 根据运行状态刷新播放或暂停图标。
     */
    if(s_sw.play_label == NULL) {
        return;
    }

    lv_label_set_text(s_sw.play_label,
                      s_sw.timer_running ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    lv_obj_align(s_sw.play_label,
                 LV_ALIGN_BOTTOM_MID,
                 0,
                 -STOPWATCH_BOTTOM_PAD);
}

static void stopwatch_display_time(int hour, int min, int sec)
{
    /* 把小时、分钟、秒分别写入 5 个标签，并重新居中布局整串时间。
     */
    char buf[4];

    snprintf(buf, sizeof(buf), "%02d", hour);
    lv_label_set_text(s_sw.countdown_hh, buf);

    lv_label_set_text(s_sw.countdown_colon_1, ":");

    snprintf(buf, sizeof(buf), "%02d", min);
    lv_label_set_text(s_sw.countdown_mm, buf);

    lv_label_set_text(s_sw.countdown_colon_2, ":");

    snprintf(buf, sizeof(buf), "%02d", sec);
    lv_label_set_text(s_sw.countdown_ss, buf);

    lv_obj_update_layout(s_sw.page);

    lv_coord_t hh_w = lv_obj_get_width(s_sw.countdown_hh);
    lv_coord_t c1_w = lv_obj_get_width(s_sw.countdown_colon_1);
    lv_coord_t mm_w = lv_obj_get_width(s_sw.countdown_mm);
    lv_coord_t c2_w = lv_obj_get_width(s_sw.countdown_colon_2);
    lv_coord_t ss_w = lv_obj_get_width(s_sw.countdown_ss);

    lv_coord_t total_w = hh_w + c1_w + mm_w + c2_w + ss_w;
    lv_coord_t x = (WATCH_SCREEN_W - total_w) / 2;
    lv_coord_t y = STOPWATCH_DISPLAY_Y;

    lv_obj_set_pos(s_sw.countdown_hh, x, y);
    x += hh_w;

    lv_obj_set_pos(s_sw.countdown_colon_1, x, y);
    x += c1_w;

    lv_obj_set_pos(s_sw.countdown_mm, x, y);
    x += mm_w;

    lv_obj_set_pos(s_sw.countdown_colon_2, x, y);
    x += c2_w;

    lv_obj_set_pos(s_sw.countdown_ss, x, y);
}

/**
 * @brief 将累计秒数格式化并显示为 HH:MM:SS。
 */
static void stopwatch_display_total(int total)
{
    /* 总秒数显示入口，先拆成 HH/MM/SS 再复用显示函数。
     */
    int hour;
    int min;
    int sec;

    stopwatch_total_seconds_to_hms(total, &hour, &min, &sec);
    stopwatch_display_time(hour, min, sec);
}

static void stopwatch_stop_finish_blink(void)
{
    /* 停止到达上限的闪烁动画并恢复边框不透明。
     */
    if(s_sw.countdown_frame == NULL) {
        return;
    }

    lv_anim_del(s_sw.countdown_frame, stopwatch_frame_opa_anim_cb);
    lv_obj_set_style_opa(s_sw.countdown_frame, LV_OPA_COVER, 0);
}

static void stopwatch_start_finish_blink(void)
{
    /* 启动到达上限闪烁动画，循环改变显示框透明度。
     */
    if(s_sw.countdown_frame == NULL) {
        return;
    }

    stopwatch_stop_finish_blink();

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_sw.countdown_frame);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a, STOPWATCH_BLINK_HALF_PERIOD_MS);
    lv_anim_set_playback_time(&a, STOPWATCH_BLINK_HALF_PERIOD_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, stopwatch_frame_opa_anim_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
}

static void stopwatch_timer_finish(void)
{
    /* 计时到达上限处理：暂停定时器、更新状态、刷新播放图标，并启动闪烁。
     */
    if(s_sw.timer) {
        lv_timer_pause(s_sw.timer);
    }

    s_sw.timer_running = false;
    s_sw.timer_finished = true;

    stopwatch_play_label_update();

    if(s_sw.focus == STOPWATCH_FOCUS_PLAY_PAUSE) {
        stopwatch_cursor_update(false);
    }

    stopwatch_start_finish_blink();
}

static void stopwatch_timer_clear_action(void)
{
    /* 清零秒表：累计时间归零，清除完成状态并恢复显示。
     */
    if(s_sw.timer) {
        lv_timer_pause(s_sw.timer);
    }

    s_sw.elapsed = 0;
    s_sw.timer_running = false;
    s_sw.timer_finished = false;

    stopwatch_stop_finish_blink();
    stopwatch_play_label_update();
    stopwatch_display_total(0);

    if(s_sw.focus == STOPWATCH_FOCUS_PLAY_PAUSE ||
       s_sw.focus == STOPWATCH_FOCUS_RESET) {
        stopwatch_cursor_update(false);
    }
}

static void stopwatch_timer_cb(lv_timer_t *timer)
{
    /* 每秒触发一次的计时回调，正计时累加并刷新显示。
     */
    (void)timer;

    if(!s_sw.timer_running) {
        return;
    }

    if(s_sw.elapsed < STOPWATCH_MAX_SECONDS) {
        s_sw.elapsed++;
    }

    stopwatch_display_total(s_sw.elapsed);

    if(s_sw.elapsed >= STOPWATCH_MAX_SECONDS) {
        stopwatch_timer_finish();
    }
}

/**
 * @brief 在启动、暂停和继续计时之间切换。
 */
static void stopwatch_timer_toggle_play_pause(void)
{
    /* 播放/暂停按钮处理入口，负责首次启动、继续、暂停和到达上限后重新开始。
     */
    if(s_sw.timer_running) {
        s_sw.timer_running = false;

        if(s_sw.timer) {
            lv_timer_pause(s_sw.timer);
        }

        stopwatch_play_label_update();
        stopwatch_cursor_update(false);
        return;
    }

    /* 若已到达上限，先清零再重新启动。 */
    if(s_sw.timer_finished) {
        stopwatch_timer_clear_action();
    }

    s_sw.timer_running = true;
    s_sw.timer_finished = false;

    stopwatch_stop_finish_blink();
    stopwatch_play_label_update();
    stopwatch_cursor_update(false);

    if(s_sw.timer) {
        lv_timer_reset(s_sw.timer);
        lv_timer_resume(s_sw.timer);
    }
}

void watch_tomato_clock_destroy(void);

/**
 * @brief 创建秒表页面。
 */
lv_obj_t *watch_tomato_clock_create(lv_obj_t *parent)
{
    if(s_sw.page != NULL || s_sw.timer != NULL) {
        watch_tomato_clock_destroy();
    }

    memset(&s_sw, 0, sizeof(s_sw));

    s_sw.page = lv_obj_create(parent);
    lv_obj_remove_style_all(s_sw.page);
    lv_obj_set_size(s_sw.page, WATCH_SCREEN_W, WATCH_SCREEN_H);
    lv_obj_set_pos(s_sw.page, 0, 0);
    lv_obj_set_style_bg_color(s_sw.page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_sw.page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_sw.page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_sw.page, LV_OBJ_FLAG_CLICKABLE);

    s_sw.back_btn = lv_label_create(s_sw.page);
    stopwatch_label_style_26(s_sw.back_btn);
    lv_label_set_text(s_sw.back_btn, LV_SYMBOL_LEFT);
    lv_obj_set_pos(s_sw.back_btn, 12, STOPWATCH_TOP_Y);

    s_sw.reset_bg = lv_obj_create(s_sw.page);
    lv_obj_remove_style_all(s_sw.reset_bg);
    lv_obj_set_style_bg_color(s_sw.reset_bg, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_sw.reset_bg, LV_OPA_30, 0);
    lv_obj_set_style_radius(s_sw.reset_bg, 8, 0);
    lv_obj_clear_flag(s_sw.reset_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_sw.reset_bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_sw.reset_bg, LV_OBJ_FLAG_HIDDEN);

    s_sw.play_label = lv_label_create(s_sw.page);
    stopwatch_label_style_26(s_sw.play_label);
    lv_label_set_text(s_sw.play_label, LV_SYMBOL_PLAY);
    lv_obj_align(s_sw.play_label,
                 LV_ALIGN_BOTTOM_MID,
                 0,
                 -STOPWATCH_BOTTOM_PAD);

    s_sw.reset_label = lv_label_create(s_sw.page);
    stopwatch_label_style_26(s_sw.reset_label);
    lv_label_set_text(s_sw.reset_label, "RST");
    lv_obj_align(s_sw.reset_label,
             LV_ALIGN_BOTTOM_RIGHT,
             -STOPWATCH_RESET_X,
             -STOPWATCH_RESET_Y);

    s_sw.countdown_frame = lv_obj_create(s_sw.page);
    lv_obj_remove_style_all(s_sw.countdown_frame);

    lv_obj_set_size(s_sw.countdown_frame,STOPWATCH_COUNTDOWN_FRAME_W,STOPWATCH_COUNTDOWN_FRAME_H);

    lv_obj_set_pos(s_sw.countdown_frame,STOPWATCH_COUNTDOWN_FRAME_X,STOPWATCH_COUNTDOWN_FRAME_Y);

    lv_obj_set_style_bg_opa(s_sw.countdown_frame, LV_OPA_TRANSP, 0);

    lv_obj_set_style_border_color(s_sw.countdown_frame, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_border_opa(s_sw.countdown_frame, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_sw.countdown_frame,STOPWATCH_COUNTDOWN_FRAME_BORDER_W,0);

    lv_obj_set_style_radius(s_sw.countdown_frame,STOPWATCH_COUNTDOWN_FRAME_RADIUS,0);
    lv_obj_set_style_opa(s_sw.countdown_frame, LV_OPA_COVER, 0);

    lv_obj_clear_flag(s_sw.countdown_frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_sw.countdown_frame, LV_OBJ_FLAG_CLICKABLE);

    s_sw.countdown_hh = lv_label_create(s_sw.page);
    s_sw.countdown_colon_1 = lv_label_create(s_sw.page);
    s_sw.countdown_mm = lv_label_create(s_sw.page);
    s_sw.countdown_colon_2 = lv_label_create(s_sw.page);
    s_sw.countdown_ss = lv_label_create(s_sw.page);

    lv_obj_t *countdown_labels[] = {
        s_sw.countdown_hh,
        s_sw.countdown_colon_1,
        s_sw.countdown_mm,
        s_sw.countdown_colon_2,
        s_sw.countdown_ss,
    };

    for(int i = 0; i < 5; i++) {
        lv_obj_set_style_text_font(countdown_labels[i], &lv_font_montserrat_40, 0);
        lv_obj_set_style_text_color(countdown_labels[i], lv_color_white(), 0);
        lv_obj_set_style_text_align(countdown_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_clear_flag(countdown_labels[i], LV_OBJ_FLAG_CLICKABLE);
    }

    s_sw.cursor = lv_obj_create(s_sw.page);
    lv_obj_remove_style_all(s_sw.cursor);
    lv_obj_set_size(s_sw.cursor, 20, 20);
    lv_obj_set_style_bg_opa(s_sw.cursor, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_sw.cursor, lv_color_white(), 0);
    lv_obj_set_style_border_opa(s_sw.cursor, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_sw.cursor, STOPWATCH_SELECTOR_BORDER_W, 0);
    lv_obj_set_style_radius(s_sw.cursor, STOPWATCH_SELECTOR_RADIUS, 0);
    lv_obj_set_style_pad_all(s_sw.cursor, 0, 0);
    lv_obj_clear_flag(s_sw.cursor, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_sw.cursor, LV_OBJ_FLAG_CLICKABLE);

    s_sw.timer = lv_timer_create(stopwatch_timer_cb, STOPWATCH_TIMER_PERIOD_MS, NULL);
    lv_timer_pause(s_sw.timer);

    s_sw.focus = STOPWATCH_FOCUS_BACK;
    s_sw.wants_back = false;
    s_sw.timer_running = false;
    s_sw.timer_finished = false;
    s_sw.reset_pressed = false;
    s_sw.elapsed = 0;

    stopwatch_display_total(0);
    stopwatch_apply_language(true);
    stopwatch_cursor_update(false);

    return s_sw.page;
}

/**
 * @brief 重置秒表页面状态。
 */
void watch_tomato_clock_reset(void)
{
    /* 页面进入时复位秒表 UI 和状态，保证每次进入都有确定初始焦点。
     */
    if(s_sw.page == NULL) {
        return;
    }

    s_sw.focus = STOPWATCH_FOCUS_BACK;
    s_sw.wants_back = false;
    s_sw.timer_running = false;
    s_sw.timer_finished = false;
    s_sw.reset_pressed = false;
    s_sw.elapsed = 0;

    if(s_sw.timer) {
        lv_timer_pause(s_sw.timer);
    }

    stopwatch_stop_finish_blink();
    stopwatch_play_label_update();
    stopwatch_display_total(0);
    stopwatch_apply_language(true);
    lv_obj_clear_flag(s_sw.cursor, LV_OBJ_FLAG_HIDDEN);
    stopwatch_cursor_update(false);
}

/**
 * @brief 销毁秒表页面和定时器。
 */
void watch_tomato_clock_destroy(void)
{
    /* 销毁秒表定时器和页面对象引用，避免页面离开后定时器继续回调。
     */
    if(s_sw.timer) {
        lv_timer_delete(s_sw.timer);
        s_sw.timer = NULL;
    }

    if(s_sw.cursor) {
        lv_anim_del(s_sw.cursor, stopwatch_cursor_x_anim_cb);
        lv_anim_del(s_sw.cursor, stopwatch_cursor_y_anim_cb);
        lv_anim_del(s_sw.cursor, stopwatch_cursor_w_anim_cb);
        lv_anim_del(s_sw.cursor, stopwatch_cursor_h_anim_cb);
    }

    if(s_sw.countdown_frame) {
        lv_anim_del(s_sw.countdown_frame, stopwatch_frame_opa_anim_cb);
    }

    if(s_sw.page) {
        lv_obj_del(s_sw.page);
        s_sw.page = NULL;
    }

    memset(&s_sw, 0, sizeof(s_sw));
}

/**
 * @brief 处理秒表页面按键事件。
 */
void watch_tomato_clock_on_key(watch_key_t key)
{
    /* 秒表按键状态机，焦点在 返回/启动暂停/重置 间循环。
     *
     * KEY1：焦点向左移动
     * KEY3：焦点向右移动
     * KEY2：确认当前焦点对应操作
     */
    if(s_sw.page == NULL) {
        return;
    }

    stopwatch_apply_language(false);

    if(key == WATCH_KEY_2_RELEASE) {
        if(s_sw.reset_pressed) {
            s_sw.reset_pressed = false;
            stopwatch_reset_bg_update();
        }
        return;
    }

    if(key == WATCH_KEY_3) {
        if(s_sw.focus == STOPWATCH_FOCUS_BACK) {
            s_sw.focus = STOPWATCH_FOCUS_PLAY_PAUSE;
        }
        else if(s_sw.focus == STOPWATCH_FOCUS_PLAY_PAUSE) {
            s_sw.focus = STOPWATCH_FOCUS_RESET;
        }
        else if(s_sw.focus == STOPWATCH_FOCUS_RESET) {
            s_sw.focus = STOPWATCH_FOCUS_BACK;
        }
        else {
            s_sw.focus = STOPWATCH_FOCUS_BACK;
        }

        stopwatch_cursor_update(true);
        return;
    }

    if(key == WATCH_KEY_1) {
        if(s_sw.focus == STOPWATCH_FOCUS_PLAY_PAUSE) {
            s_sw.focus = STOPWATCH_FOCUS_BACK;
        }
        else if(s_sw.focus == STOPWATCH_FOCUS_RESET) {
            s_sw.focus = STOPWATCH_FOCUS_PLAY_PAUSE;
        }
        else if(s_sw.focus == STOPWATCH_FOCUS_BACK) {
            /* BACK 下按 KEY1：不再向左移动。 */
            return;
        }
        else {
            s_sw.focus = STOPWATCH_FOCUS_BACK;
        }

        stopwatch_cursor_update(true);
        return;
    }

    if(key == WATCH_KEY_2) {
        if(s_sw.focus == STOPWATCH_FOCUS_BACK) {
            s_sw.wants_back = true;
        }
        else if(s_sw.focus == STOPWATCH_FOCUS_PLAY_PAUSE) {
            stopwatch_timer_toggle_play_pause();
        }
        else if(s_sw.focus == STOPWATCH_FOCUS_RESET) {
            s_sw.reset_pressed = true;
            stopwatch_reset_bg_update();
            stopwatch_timer_clear_action();
        }

        return;
    }
}

/**
 * @brief 查询秒表页面是否请求返回。
 */
bool watch_tomato_clock_wants_back(void)
{
    /* 供 UI 调度器判断秒表页是否请求返回菜单。
     */
    return s_sw.wants_back;
}


/* 维护提示
 * 秒表交互只有一个焦点环：返回 → 启动暂停 → 重置 → 返回。
 */
