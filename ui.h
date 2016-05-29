/*
 * Carliv Touch Recovery 2016
 */

#ifndef RECOVERY_UI_H_
#define RECOVERY_UI_H_

#include "common.h"

#define MAX_COLS 96
#define MAX_ROWS 32
#define MENU_MAX_COLS 64
#define MENU_MAX_ROWS 250
#define MENU_ITEM_HEADER " - "
#define MENU_ITEM_HEADER_LENGTH strlen(MENU_ITEM_HEADER)

#define MIN_LOG_ROWS 3

#define CHAR_WIDTH BOARD_RECOVERY_CHAR_WIDTH
#define CHAR_HEIGHT BOARD_RECOVERY_CHAR_HEIGHT

// Delay in seconds to refresh clock and USB plugged volumes
#define REFRESH_TIME_USB_INTERVAL 5

#define UI_WAIT_KEY_TIMEOUT_SEC    3600
#define UI_KEY_REPEAT_INTERVAL     80
#define UI_KEY_WAIT_REPEAT         400
#define UI_MIN_PROG_DELTA_MS       200

#define VIBRATOR_TIME_MS    40

#define TOUCH_RESET_POS -10000

#define LEFT_ALIGN   0
#define CENTER_ALIGN 1
#define RIGHT_ALIGN  2

#define MENU_TEXT_COLOR         242, 241, 239, 255
#define MENU_SELECTED_COLOR     255, 255, 255, 255
#define NORMAL_TEXT_COLOR       230, 230, 230, 255
#define HEADER_TEXT_COLOR       34, 167, 240, 255
#define MENU_BACKGROUND_COLOR   50, 50, 50, 160
#define MENU_HIGHLIGHT_COLOR    8, 157, 227, 255
#define MENU_SEPARATOR_COLOR    34, 167, 240, 70

#define MENU_CHAR_HEIGHT        ((CHAR_HEIGHT) - ((CHAR_HEIGHT) % 4))
#define MENU_TOTAL_HEIGHT       ((CHAR_HEIGHT) + ((MENU_CHAR_HEIGHT) * 1.6))

#define MAX_DEVICES 16
#define MAX_MISC_FDS 16

#define SCROLL_SENSITIVITY    ((MENU_TOTAL_HEIGHT) / 4)

#endif  // RECOVERY_UI_H_
