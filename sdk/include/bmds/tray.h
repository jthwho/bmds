/**
 * @file      tray.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system tray's messages (devplan/mediaplayer.md §6e, devplan/guiprograms.md §12 4d), sent on
 * the service bus (`Services`, the SERVICES extension): a program's tray icon sends to the tray's
 * service `tray`, and the tray answers to the program's reply service (`reply`, "tray.N"). A
 * program with a `TrayIcon` object needs none of this: it is for a program that is the tray (a
 * panel of its own), or an icon without the class.
 *
 * Each message is one of these structs, sent whole, starting with `size` (the struct's size as
 * its sender knew it) and `kind`. Fields are appended, never changed or moved: read what was sent
 * and take the fields past it as zero. Strings end in a NUL within their arrays (or fill them: read
 * them up to their size). The same layout as the system's `TrayProtocol` (bmds/trayprotocol.h).
 */

#ifndef BMDS_TRAY_H
#define BMDS_TRAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The tray's service. */
#define BMDS_TRAY_SERVICE "tray"

/** @brief `kind`: to the tray. */
#define BMDS_TRAY_UPDATE 1u /**< An icon added or changed, all of it: `bmds_tray_update_t`. */
#define BMDS_TRAY_PIXELS 2u /**< An icon's own pixels: `bmds_tray_pixels_t`. */
#define BMDS_TRAY_REMOVE 3u /**< An icon gone: `bmds_tray_header_t`. */

/** @brief `kind`: to a program (`bmds_tray_event_t`). */
#define BMDS_TRAY_ACTIVATED 101u /**< Clicked: `reason`, the icon's rectangle. */
#define BMDS_TRAY_SCROLLED 102u  /**< The wheel over it: `notches`, the rectangle. */
#define BMDS_TRAY_GEOMETRY 103u  /**< Its place on the screen, when it changes. */

/** @brief `category`: where the tray puts an icon (by category, then in the order they came). */
#define BMDS_TRAY_APPLICATION 0u
#define BMDS_TRAY_COMMUNICATIONS 1u
#define BMDS_TRAY_SYSTEM 2u
#define BMDS_TRAY_HARDWARE 3u /**< At the right end (the volume). */

/** @brief `reason` of `BMDS_TRAY_ACTIVATED`. */
#define BMDS_TRAY_TRIGGER 1u      /**< A left click. */
#define BMDS_TRAY_DOUBLE_CLICK 2u /**< A second left click soon after (a trigger came first). */
#define BMDS_TRAY_MIDDLE_CLICK 3u
#define BMDS_TRAY_CONTEXT 4u /**< A right click: the program opens its menu at the rectangle. */

/** @brief `flags` of `BMDS_TRAY_UPDATE`. */
#define BMDS_TRAY_VISIBLE 0x1u    /**< Shown (else kept, hidden). */
#define BMDS_TRAY_HAS_PIXELS 0x2u /**< Its pixels follow (`BMDS_TRAY_PIXELS`); `icon` unused. */

/** @brief An icon's side in pixels. */
#define BMDS_TRAY_ICON_SIDE 16u

/** @brief What every message to the tray starts with (`BMDS_TRAY_REMOVE` is only this). */
typedef struct bmds_tray_header_t {
                uint32_t size;
                uint32_t kind;
                uint32_t id;        /**< The icon's number, the program's own. */
                char     reply[32]; /**< The program's reply service. */
} bmds_tray_header_t;

/** @brief An icon added or changed (all of it, each time). */
typedef struct bmds_tray_update_t {
                uint32_t size;
                uint32_t kind;
                uint32_t id;
                char     reply[32];
                uint32_t category;
                uint32_t flags;
                char     icon[64]; /**< A theme's icon name. */
                char     tool_tip[128];
                char     tool_tip_more[256]; /**< The tooltip past `tool_tip`'s 127 bytes. */
} bmds_tray_update_t;

/** @brief An icon's own pixels: 0xAARRGGBB (straight alpha), row by row. */
typedef struct bmds_tray_pixels_t {
                uint32_t size;
                uint32_t kind;
                uint32_t id;
                char     reply[32];
                uint32_t width; /**< `BMDS_TRAY_ICON_SIDE`. */
                uint32_t height;
                uint32_t argb[BMDS_TRAY_ICON_SIDE * BMDS_TRAY_ICON_SIDE];
} bmds_tray_pixels_t;

/** @brief What the tray tells a program. */
typedef struct bmds_tray_event_t {
                uint32_t size;
                uint32_t kind;
                uint32_t id;
                uint32_t reason;  /**< `BMDS_TRAY_ACTIVATED`'s. */
                int32_t  notches; /**< `BMDS_TRAY_SCROLLED`'s: up positive. */
                int16_t  x;       /**< The icon on the screen. */
                int16_t  y;
                uint16_t width;
                uint16_t height;
} bmds_tray_event_t;

#ifdef __cplusplus
}
#endif

#endif /* BMDS_TRAY_H */
