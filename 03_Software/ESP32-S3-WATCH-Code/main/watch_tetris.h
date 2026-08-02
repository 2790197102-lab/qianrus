/**
 * @file watch_tetris.h
 * @brief 俄罗斯方块游戏页面接口。
 */
#ifndef WATCH_TETRIS_H
#define WATCH_TETRIS_H

#include <stdbool.h>
#include "lvgl.h"
#include "watch_keys.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建俄罗斯方块游戏页面。
 *
 * @param parent LVGL 父对象。
 * @return 页面根对象，失败时返回 NULL。
 */
lv_obj_t *watch_tetris_create(lv_obj_t *parent);
/**
 * @brief 开始或恢复游戏（启动重力定时器）。
 */
void watch_tetris_start(void);
/**
 * @brief 暂停游戏（暂停重力定时器）。
 */
void watch_tetris_stop(void);
/**
 * @brief 处理按键事件。
 *
 * 按键映射：
 * - KEY1：左移
 * - KEY2：旋转
 * - KEY3：右移
 * - 游戏结束后 KEY2：返回上级
 *
 * @param key 按键事件。
 */
void watch_tetris_on_key(watch_key_t key);
/**
 * @brief 判断是否请求返回上级页面。
 *
 * @return true 请求返回；false 继续游戏。
 */
bool watch_tetris_wants_back(void);
/**
 * @brief 判断游戏是否已经结束。
 *
 * @return true 已结束；false 进行中。
 */
bool watch_tetris_is_game_over(void);
/**
 * @brief 获取当前分数。
 *
 * @return 当前分数。
 */
int watch_tetris_get_score(void);
/**
 * @brief 游戏结束回调类型。
 */
typedef void (*watch_tetris_game_over_cb_t)(void);
/**
 * @brief 设置游戏结束回调。
 *
 * @param cb 游戏结束时调用的回调，传 NULL 可取消。
 */
void watch_tetris_set_game_over_cb(watch_tetris_game_over_cb_t cb);
/**
 * @brief 销毁游戏页面、定时器等所有资源。
 */
void watch_tetris_destroy(void);

#ifdef __cplusplus
}
#endif

#endif
