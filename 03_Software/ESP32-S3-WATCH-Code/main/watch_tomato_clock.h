/**
 * @file watch_tomato_clock.h
 * @brief 秒表页面对外接口。
 *
 * 说明：文件名保留 watch_tomato_clock 是为避免上层 (watch_ui / watch_menu / CMakeLists)
 * 大范围重命名，实际功能已由番茄钟改造为秒表。
 */

#ifndef WATCH_TOMATO_CLOCK_H
#define WATCH_TOMATO_CLOCK_H

#include <stdbool.h>
#include "lvgl.h"
#include "watch_keys.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建秒表页面。
 *
 * @param parent 父对象。
 * @return lv_obj_t* 页面根对象，失败返回 NULL。
 */
lv_obj_t *watch_tomato_clock_create(lv_obj_t *parent);
/**
 * @brief 重置秒表页面状态。
 */
void watch_tomato_clock_reset(void);
/**
 * @brief 处理秒表页面按键事件。
 *
 * @param key 按键值。
 */
void watch_tomato_clock_on_key(watch_key_t key);
/**
 * @brief 判断秒表页面是否请求返回。
 *
 * @return true 请求返回。
 */
bool watch_tomato_clock_wants_back(void);
/**
 * @brief 销毁秒表页面及相关定时器。
 */
void watch_tomato_clock_destroy(void);

#ifdef __cplusplus
}
#endif

#endif
