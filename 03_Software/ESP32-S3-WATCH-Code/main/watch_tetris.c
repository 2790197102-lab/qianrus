/**
 * @file watch_tetris.c
 * @brief 俄罗斯方块游戏：棋盘、7 种方块、旋转、消行、计分与重力状态机。
 *
 * 设计说明：
 * - 棋盘 10 列 × 20 行，每格 10×10 像素，位于屏幕左侧；
 * - 右侧显示下一方块预览、分数、消行数和等级；
 * - 重力由 lv_timer 驱动，间隔随等级提升而缩短；
 * - 渲染采用固定 200 个格子对象，根据 grid 状态切换颜色/透明度，
 *   下落中的方块合并到显示但不写入 grid，落地后才写入。
 */

#include "watch_tetris.h"
#include "watch_language.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

LV_FONT_DECLARE(cn_font_26);

/* ======================== 布局参数 ======================== */
#define WATCH_SCREEN_W          240
#define WATCH_SCREEN_H          240

#define TETRIS_COLS             10
#define TETRIS_ROWS             20
#define TETRIS_CELL_SIZE        10

#define TETRIS_BOARD_X          16
#define TETRIS_BOARD_Y          20
#define TETRIS_BOARD_W          (TETRIS_COLS * TETRIS_CELL_SIZE)   /* 100 */
#define TETRIS_BOARD_H          (TETRIS_ROWS * TETRIS_CELL_SIZE)   /* 200 */

#define TETRIS_INFO_X           (TETRIS_BOARD_X + TETRIS_BOARD_W + 14) /* 130 */
#define TETRIS_PREVIEW_X        TETRIS_INFO_X
#define TETRIS_PREVIEW_Y        40
#define TETRIS_PREVIEW_CELL     8
#define TETRIS_PREVIEW_SIZE     (4 * TETRIS_PREVIEW_CELL)          /* 32 */

#define TETRIS_LABEL_Y_NEXT     24
#define TETRIS_LABEL_Y_SCORE    88
#define TETRIS_VALUE_Y_SCORE    108
#define TETRIS_LABEL_Y_LINES    132
#define TETRIS_VALUE_Y_LINES    152
#define TETRIS_LABEL_Y_LEVEL    176
#define TETRIS_VALUE_Y_LEVEL    196

/* ======================== 游戏参数 ======================== */
#define TETRIS_GRAVITY_BASE_MS  800
#define TETRIS_GRAVITY_MIN_MS   120
#define TETRIS_GRAVITY_STEP_MS  70

/* 方块类型：0=空，1..7 对应 7 种方块 */
#define TETRIS_SHAPE_NONE       0
#define TETRIS_SHAPE_COUNT      7

/* ======================== 方块形状定义 ========================
 * 每种方块 4 个旋转状态，每个状态用 4×4 矩阵表示。
 * 1 表示该格被填充。
 */
static const int SHAPE_MATRICES[TETRIS_SHAPE_COUNT][4][4][4] = {
    /* I */
    {
        {{0,0,0,0},{1,1,1,1},{0,0,0,0},{0,0,0,0}},
        {{0,0,1,0},{0,0,1,0},{0,0,1,0},{0,0,1,0}},
        {{0,0,0,0},{0,0,0,0},{1,1,1,1},{0,0,0,0}},
        {{0,1,0,0},{0,1,0,0},{0,1,0,0},{0,1,0,0}},
    },
    /* O */
    {
        {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},
        {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},
        {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},
        {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},
    },
    /* T */
    {
        {{0,1,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},
        {{0,1,0,0},{0,1,1,0},{0,1,0,0},{0,0,0,0}},
        {{0,0,0,0},{1,1,1,0},{0,1,0,0},{0,0,0,0}},
        {{0,1,0,0},{1,1,0,0},{0,1,0,0},{0,0,0,0}},
    },
    /* S */
    {
        {{0,1,1,0},{1,1,0,0},{0,0,0,0},{0,0,0,0}},
        {{0,1,0,0},{0,1,1,0},{0,0,1,0},{0,0,0,0}},
        {{0,0,0,0},{0,1,1,0},{1,1,0,0},{0,0,0,0}},
        {{1,0,0,0},{1,1,0,0},{0,1,0,0},{0,0,0,0}},
    },
    /* Z */
    {
        {{1,1,0,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},
        {{0,0,1,0},{0,1,1,0},{0,1,0,0},{0,0,0,0}},
        {{0,0,0,0},{1,1,0,0},{0,1,1,0},{0,0,0,0}},
        {{0,1,0,0},{1,1,0,0},{1,0,0,0},{0,0,0,0}},
    },
    /* J */
    {
        {{1,0,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},
        {{0,1,1,0},{0,1,0,0},{0,1,0,0},{0,0,0,0}},
        {{0,0,0,0},{1,1,1,0},{0,0,1,0},{0,0,0,0}},
        {{0,1,0,0},{0,1,0,0},{1,1,0,0},{0,0,0,0}},
    },
    /* L */
    {
        {{0,0,1,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},
        {{0,1,0,0},{0,1,0,0},{0,1,1,0},{0,0,0,0}},
        {{0,0,0,0},{1,1,1,0},{1,0,0,0},{0,0,0,0}},
        {{1,1,0,0},{0,1,0,0},{0,1,0,0},{0,0,0,0}},
    },
};

/* 7 种方块颜色（索引 0..6 对应方块 1..7） */
static const lv_color_t SHAPE_COLORS[TETRIS_SHAPE_COUNT] = {
    LV_COLOR_MAKE(0x00, 0xF0, 0xF0),  /* I 青 */
    LV_COLOR_MAKE(0xF0, 0xF0, 0x00),  /* O 黄 */
    LV_COLOR_MAKE(0xC0, 0x00, 0xF0),  /* T 紫 */
    LV_COLOR_MAKE(0x00, 0xF0, 0x00),  /* S 绿 */
    LV_COLOR_MAKE(0xF0, 0x00, 0x00),  /* Z 红 */
    LV_COLOR_MAKE(0x00, 0x00, 0xF0),  /* J 蓝 */
    LV_COLOR_MAKE(0xF0, 0xA0, 0x00),  /* L 橙 */
};

/* ======================== 上下文 ======================== */
typedef struct {
    lv_obj_t *page;
    lv_obj_t *board_bg;            /* 棋盘边框背景 */
    lv_obj_t *cells[TETRIS_ROWS][TETRIS_COLS];  /* 200 个格子 */
    lv_obj_t *preview_cells[4][4]; /* 下一方块预览 */
    lv_obj_t *label_next;
    lv_obj_t *label_score;
    lv_obj_t *value_score;
    lv_obj_t *label_lines;
    lv_obj_t *value_lines;
    lv_obj_t *label_level;
    lv_obj_t *value_level;
    lv_obj_t *game_over_label;

    lv_timer_t *timer;

    /* 游戏状态 */
    int grid[TETRIS_ROWS][TETRIS_COLS];  /* 0 空，1..7 方块类型 */
    int cur_type;     /* 1..7 */
    int cur_rotation; /* 0..3 */
    int cur_x;        /* 方块左上角列 */
    int cur_y;        /* 方块左上角行 */
    int next_type;    /* 1..7 */

    int score;
    int lines;
    int level;

    bool running;
    bool game_over;
    bool wants_back;

    watch_tetris_game_over_cb_t over_cb;
} tetris_ctx_t;

static tetris_ctx_t s_t;

/* ======================== 辅助函数 ======================== */
static int tetris_gravity_ms(void)
{
    /* 根据等级计算重力间隔，越高越快。 */
    int ms = TETRIS_GRAVITY_BASE_MS - (s_t.level - 1) * TETRIS_GRAVITY_STEP_MS;
    return ms < TETRIS_GRAVITY_MIN_MS ? TETRIS_GRAVITY_MIN_MS : ms;
}

static int tetris_shape_cell(int type, int rot, int row, int col)
{
    /* 读取方块矩阵指定位置，type 为 1..7，转索引 0..6。 */
    if(type < 1 || type > TETRIS_SHAPE_COUNT) {
        return 0;
    }
    return SHAPE_MATRICES[type - 1][rot & 3][row & 3][col & 3];
}

static bool tetris_can_place(int type, int rot, int x, int y)
{
    /* 检查方块在 (x,y) 处是否可以放置（不越界、不冲突）。 */
    for(int r = 0; r < 4; r++) {
        for(int c = 0; c < 4; c++) {
            if(!tetris_shape_cell(type, rot, r, c)) {
                continue;
            }
            int gx = x + c;
            int gy = y + r;
            if(gx < 0 || gx >= TETRIS_COLS || gy >= TETRIS_ROWS) {
                return false;
            }
            if(gy < 0) {
                continue; /* 顶部上方允许 */
            }
            if(s_t.grid[gy][gx] != TETRIS_SHAPE_NONE) {
                return false;
            }
        }
    }
    return true;
}

static void tetris_lock_piece(void)
{
    /* 把当前方块写入 grid。 */
    for(int r = 0; r < 4; r++) {
        for(int c = 0; c < 4; c++) {
            if(!tetris_shape_cell(s_t.cur_type, s_t.cur_rotation, r, c)) {
                continue;
            }
            int gx = s_t.cur_x + c;
            int gy = s_t.cur_y + r;
            if(gy >= 0 && gy < TETRIS_ROWS && gx >= 0 && gx < TETRIS_COLS) {
                s_t.grid[gy][gx] = s_t.cur_type;
            }
        }
    }
}

static void tetris_clear_lines(void)
{
    /* 消除满行并下移上方方块，更新分数/行数/等级。 */
    int cleared = 0;

    for(int r = TETRIS_ROWS - 1; r >= 0; r--) {
        bool full = true;
        for(int c = 0; c < TETRIS_COLS; c++) {
            if(s_t.grid[r][c] == TETRIS_SHAPE_NONE) {
                full = false;
                break;
            }
        }

        if(full) {
            cleared++;
            for(int rr = r; rr > 0; rr--) {
                for(int c = 0; c < TETRIS_COLS; c++) {
                    s_t.grid[rr][c] = s_t.grid[rr - 1][c];
                }
            }
            for(int c = 0; c < TETRIS_COLS; c++) {
                s_t.grid[0][c] = TETRIS_SHAPE_NONE;
            }
            r++; /* 同一行重新检查 */
        }
    }

    if(cleared > 0) {
        static const int SCORE_TABLE[5] = {0, 10, 30, 50, 80};
        s_t.score += SCORE_TABLE[cleared] * s_t.level;
        s_t.lines += cleared;
        s_t.level = s_t.lines / 10 + 1;
    }
}

static void tetris_spawn_piece(void)
{
    /* 生成新方块，若放置失败则游戏结束。 */
    s_t.cur_type = s_t.next_type;
    s_t.cur_rotation = 0;
    s_t.cur_x = (TETRIS_COLS - 4) / 2;
    s_t.cur_y = -1; /* 略微露出顶部 */
    s_t.next_type = (rand() % TETRIS_SHAPE_COUNT) + 1;

    if(!tetris_can_place(s_t.cur_type, s_t.cur_rotation, s_t.cur_x, s_t.cur_y + 1)) {
        /* 顶部即冲突，游戏结束 */
        s_t.game_over = true;
        s_t.running = false;
        if(s_t.timer) {
            lv_timer_pause(s_t.timer);
        }
        if(s_t.over_cb) {
            s_t.over_cb();
        }
    }
}

/* ======================== 渲染 ======================== */
static void tetris_render_board(void)
{
    /* 刷新棋盘 200 个格子，合并当前下落方块到显示。 */
    for(int r = 0; r < TETRIS_ROWS; r++) {
        for(int c = 0; c < TETRIS_COLS; c++) {
            lv_obj_t *cell = s_t.cells[r][c];
            int val = s_t.grid[r][c];

            /* 合并当前方块显示 */
            if(!s_t.game_over && val == TETRIS_SHAPE_NONE) {
                int br = r - s_t.cur_y;
                int bc = c - s_t.cur_x;
                if(br >= 0 && br < 4 && bc >= 0 && bc < 4 &&
                   tetris_shape_cell(s_t.cur_type, s_t.cur_rotation, br, bc)) {
                    val = s_t.cur_type;
                }
            }

            if(val > TETRIS_SHAPE_NONE) {
                lv_obj_set_style_bg_color(cell, SHAPE_COLORS[val - 1], 0);
                lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            } else {
                lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
            }
        }
    }
}

static void tetris_render_preview(void)
{
    /* 刷新下一方块预览 4×4 格子。 */
    for(int r = 0; r < 4; r++) {
        for(int c = 0; c < 4; c++) {
            lv_obj_t *cell = s_t.preview_cells[r][c];
            if(tetris_shape_cell(s_t.next_type, 0, r, c)) {
                lv_obj_set_style_bg_color(cell, SHAPE_COLORS[s_t.next_type - 1], 0);
                lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            } else {
                lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
            }
        }
    }
}

static void tetris_render_info(void)
{
    char buf[16];

    if(s_t.value_score) {
        snprintf(buf, sizeof(buf), "%d", s_t.score);
        lv_label_set_text(s_t.value_score, buf);
    }
    if(s_t.value_lines) {
        snprintf(buf, sizeof(buf), "%d", s_t.lines);
        lv_label_set_text(s_t.value_lines, buf);
    }
    if(s_t.value_level) {
        snprintf(buf, sizeof(buf), "%d", s_t.level);
        lv_label_set_text(s_t.value_level, buf);
    }
}

static void tetris_render_all(void)
{
    tetris_render_board();
    tetris_render_preview();
    tetris_render_info();
}

static void tetris_update_game_over_label(void)
{
    if(s_t.game_over_label == NULL) {
        return;
    }
    if(s_t.game_over) {
        lv_obj_clear_flag(s_t.game_over_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_t.game_over_label, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ======================== 方块操作 ======================== */
static bool tetris_try_move(int dx, int dy)
{
    /* 尝试水平/垂直移动，成功返回 true。 */
    if(tetris_can_place(s_t.cur_type, s_t.cur_rotation, s_t.cur_x + dx, s_t.cur_y + dy)) {
        s_t.cur_x += dx;
        s_t.cur_y += dy;
        return true;
    }
    return false;
}

static void tetris_try_rotate(void)
{
    /* 尝试顺时针旋转，带简单踢墙。 */
    int new_rot = (s_t.cur_rotation + 1) & 3;
    static const int kicks[5][2] = {{0,0},{-1,0},{1,0},{0,-1},{-2,0}};

    for(int i = 0; i < 5; i++) {
        if(tetris_can_place(s_t.cur_type, new_rot, s_t.cur_x + kicks[i][0], s_t.cur_y + kicks[i][1])) {
            s_t.cur_rotation = new_rot;
            s_t.cur_x += kicks[i][0];
            s_t.cur_y += kicks[i][1];
            return;
        }
    }
}

static void tetris_soft_drop(void)
{
    /* 重力下落一格，落地则锁定并消行、生成新方块。 */
    if(!tetris_try_move(0, 1)) {
        tetris_lock_piece();
        tetris_clear_lines();
        tetris_spawn_piece();
    }
}

/* ======================== 定时器 ======================== */
static void tetris_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if(!s_t.running || s_t.game_over) {
        return;
    }

    tetris_soft_drop();
    tetris_render_all();
    tetris_update_game_over_label();

    /* 根据等级调整重力间隔 */
    if(s_t.timer) {
        lv_timer_set_period(s_t.timer, tetris_gravity_ms());
    }
}

/* ======================== 对外接口 ======================== */
void watch_tetris_destroy(void);

static void tetris_reset_state(void)
{
    /* 重置游戏逻辑状态到初始。 */
    memset(s_t.grid, 0, sizeof(s_t.grid));
    s_t.score = 0;
    s_t.lines = 0;
    s_t.level = 1;
    s_t.game_over = false;
    s_t.running = false;
    s_t.wants_back = false;
    s_t.next_type = (rand() % TETRIS_SHAPE_COUNT) + 1;
    tetris_spawn_piece();
}

lv_obj_t *watch_tetris_create(lv_obj_t *parent)
{
    if(s_t.page != NULL) {
        watch_tetris_destroy();
    }
    memset(&s_t, 0, sizeof(s_t));

    s_t.page = lv_obj_create(parent);
    lv_obj_remove_style_all(s_t.page);
    lv_obj_set_size(s_t.page, WATCH_SCREEN_W, WATCH_SCREEN_H);
    lv_obj_set_pos(s_t.page, 0, 0);
    lv_obj_set_style_bg_color(s_t.page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_t.page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_t.page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_t.page, LV_OBJ_FLAG_CLICKABLE);

    /* 棋盘边框 */
    s_t.board_bg = lv_obj_create(s_t.page);
    lv_obj_remove_style_all(s_t.board_bg);
    lv_obj_set_size(s_t.board_bg, TETRIS_BOARD_W + 4, TETRIS_BOARD_H + 4);
    lv_obj_set_pos(s_t.board_bg, TETRIS_BOARD_X - 2, TETRIS_BOARD_Y - 2);
    lv_obj_set_style_bg_color(s_t.board_bg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_t.board_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_t.board_bg, lv_color_white(), 0);
    lv_obj_set_style_border_opa(s_t.board_bg, LV_OPA_40, 0);
    lv_obj_set_style_border_width(s_t.board_bg, 1, 0);
    lv_obj_set_style_radius(s_t.board_bg, 2, 0);
    lv_obj_set_style_pad_all(s_t.board_bg, 0, 0);
    lv_obj_clear_flag(s_t.board_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_t.board_bg, LV_OBJ_FLAG_CLICKABLE);

    /* 200 个棋盘格子 */
    for(int r = 0; r < TETRIS_ROWS; r++) {
        for(int c = 0; c < TETRIS_COLS; c++) {
            lv_obj_t *cell = lv_obj_create(s_t.page);
            lv_obj_remove_style_all(cell);
            lv_obj_set_size(cell, TETRIS_CELL_SIZE, TETRIS_CELL_SIZE);
            lv_obj_set_pos(cell,
                           TETRIS_BOARD_X + c * TETRIS_CELL_SIZE,
                           TETRIS_BOARD_Y + r * TETRIS_CELL_SIZE);
            lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(cell, lv_color_make(0x30, 0x30, 0x30), 0);
            lv_obj_set_style_border_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(cell, 1, 0);
            lv_obj_set_style_radius(cell, 1, 0);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            s_t.cells[r][c] = cell;
        }
    }

    /* 右侧信息区标签 */
    const lv_font_t *font = watch_language_is_chinese() ? &cn_font_26 : &lv_font_montserrat_26;

    s_t.label_next = lv_label_create(s_t.page);
    lv_obj_set_style_text_font(s_t.label_next, font, 0);
    lv_obj_set_style_text_color(s_t.label_next, lv_color_white(), 0);
    lv_label_set_text(s_t.label_next, "Next");
    lv_obj_set_pos(s_t.label_next, TETRIS_INFO_X, TETRIS_LABEL_Y_NEXT);

    /* 预览 4×4 格子 */
    for(int r = 0; r < 4; r++) {
        for(int c = 0; c < 4; c++) {
            lv_obj_t *cell = lv_obj_create(s_t.page);
            lv_obj_remove_style_all(cell);
            lv_obj_set_size(cell, TETRIS_PREVIEW_CELL, TETRIS_PREVIEW_CELL);
            lv_obj_set_pos(cell,
                           TETRIS_PREVIEW_X + c * TETRIS_PREVIEW_CELL,
                           TETRIS_PREVIEW_Y + r * TETRIS_PREVIEW_CELL);
            lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(cell, lv_color_make(0x30, 0x30, 0x30), 0);
            lv_obj_set_style_border_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(cell, 1, 0);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            s_t.preview_cells[r][c] = cell;
        }
    }

    s_t.label_score = lv_label_create(s_t.page);
    lv_obj_set_style_text_font(s_t.label_score, font, 0);
    lv_obj_set_style_text_color(s_t.label_score, lv_color_white(), 0);
    lv_label_set_text(s_t.label_score, "Score");
    lv_obj_set_pos(s_t.label_score, TETRIS_INFO_X, TETRIS_LABEL_Y_SCORE);

    s_t.value_score = lv_label_create(s_t.page);
    lv_obj_set_style_text_font(s_t.value_score, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_t.value_score, lv_color_make(0x00, 0xF0, 0xF0), 0);
    lv_label_set_text(s_t.value_score, "0");
    lv_obj_set_pos(s_t.value_score, TETRIS_INFO_X, TETRIS_VALUE_Y_SCORE);

    s_t.label_lines = lv_label_create(s_t.page);
    lv_obj_set_style_text_font(s_t.label_lines, font, 0);
    lv_obj_set_style_text_color(s_t.label_lines, lv_color_white(), 0);
    lv_label_set_text(s_t.label_lines, "Lines");
    lv_obj_set_pos(s_t.label_lines, TETRIS_INFO_X, TETRIS_LABEL_Y_LINES);

    s_t.value_lines = lv_label_create(s_t.page);
    lv_obj_set_style_text_font(s_t.value_lines, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_t.value_lines, lv_color_make(0x00, 0xF0, 0x00), 0);
    lv_label_set_text(s_t.value_lines, "0");
    lv_obj_set_pos(s_t.value_lines, TETRIS_INFO_X, TETRIS_VALUE_Y_LINES);

    s_t.label_level = lv_label_create(s_t.page);
    lv_obj_set_style_text_font(s_t.label_level, font, 0);
    lv_obj_set_style_text_color(s_t.label_level, lv_color_white(), 0);
    lv_label_set_text(s_t.label_level, "Lv");
    lv_obj_set_pos(s_t.label_level, TETRIS_INFO_X, TETRIS_LABEL_Y_LEVEL);

    s_t.value_level = lv_label_create(s_t.page);
    lv_obj_set_style_text_font(s_t.value_level, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_t.value_level, lv_color_make(0xF0, 0xA0, 0x00), 0);
    lv_label_set_text(s_t.value_level, "1");
    lv_obj_set_pos(s_t.value_level, TETRIS_INFO_X + 30, TETRIS_VALUE_Y_LEVEL);

    /* Game Over 提示 */
    s_t.game_over_label = lv_label_create(s_t.page);
    lv_obj_set_style_text_font(s_t.game_over_label, font, 0);
    lv_obj_set_style_text_color(s_t.game_over_label, lv_color_make(0xF0, 0x00, 0x00), 0);
    lv_label_set_text(s_t.game_over_label, watch_language_is_chinese() ? "游戏结束" : "Game Over");
    lv_obj_center(s_t.game_over_label);
    lv_obj_add_flag(s_t.game_over_label, LV_OBJ_FLAG_HIDDEN);

    /* 重力定时器，初始暂停 */
    s_t.timer = lv_timer_create(tetris_timer_cb, TETRIS_GRAVITY_BASE_MS, NULL);
    lv_timer_pause(s_t.timer);

    tetris_reset_state();
    tetris_render_all();
    tetris_update_game_over_label();

    return s_t.page;
}

void watch_tetris_start(void)
{
    if(s_t.page == NULL || s_t.timer == NULL) {
        return;
    }
    if(s_t.game_over) {
        tetris_reset_state();
        tetris_render_all();
        tetris_update_game_over_label();
    }
    s_t.running = true;
    lv_timer_set_period(s_t.timer, tetris_gravity_ms());
    lv_timer_reset(s_t.timer);
    lv_timer_resume(s_t.timer);
}

void watch_tetris_stop(void)
{
    s_t.running = false;
    if(s_t.timer) {
        lv_timer_pause(s_t.timer);
    }
}

void watch_tetris_on_key(watch_key_t key)
{
    if(s_t.page == NULL) {
        return;
    }

    /* 游戏结束后按 KEY2 返回 */
    if(s_t.game_over) {
        if(key == WATCH_KEY_2) {
            s_t.wants_back = true;
        }
        return;
    }

    if(key == WATCH_KEY_2_RELEASE) {
        return;
    }

    if(!s_t.running) {
        return;
    }

    if(key == WATCH_KEY_1) {
        tetris_try_move(-1, 0);
        tetris_render_all();
        return;
    }

    if(key == WATCH_KEY_3) {
        tetris_try_move(1, 0);
        tetris_render_all();
        return;
    }

    if(key == WATCH_KEY_2) {
        tetris_try_rotate();
        tetris_render_all();
        return;
    }
}

bool watch_tetris_wants_back(void)
{
    return s_t.wants_back;
}

bool watch_tetris_is_game_over(void)
{
    return s_t.game_over;
}

int watch_tetris_get_score(void)
{
    return s_t.score;
}

void watch_tetris_set_game_over_cb(watch_tetris_game_over_cb_t cb)
{
    s_t.over_cb = cb;
}

void watch_tetris_destroy(void)
{
    if(s_t.timer) {
        lv_timer_delete(s_t.timer);
        s_t.timer = NULL;
    }
    if(s_t.page) {
        lv_obj_del(s_t.page);
        s_t.page = NULL;
    }
    memset(&s_t, 0, sizeof(s_t));
}
