/**
 * @file      gpio.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Linux's GPIO character device as BMDS provides it (devplan/io.md §3.9): `/dev/gpiochip0`,
 * the chip's pads, with these requests, which the SDK's `ioctl()` carries out on `gpio_ctl`
 * (`bmds/sys.h`). Written for BMDS; the layouts and numbers are Linux's
 * (`include/uapi/linux/gpio.h`), which programs were built against.
 *
 * Supported: `GPIO_GET_CHIPINFO_IOCTL`, `GPIO_GET_LINEINFO_IOCTL` and
 * `GPIO_V2_GET_LINEINFO_IOCTL` (what `gpiodetect` and `gpioinfo` ask). Taking lines (line
 * requests, values, edge events) and watching line info fail with `EOPNOTSUPP` for now: they
 * come with GPIO and muxing (devplan/io.md §6, step 3). A line's name is the board's for it
 * (`GPIO14` where the board has none); its consumer is the driver or program holding it.
 *
 * ABI: frozen. Append only.
 */

#ifndef BMDS_LINUX_GPIO_H
#define BMDS_LINUX_GPIO_H

#include <stdint.h>
#include <sys/ioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Room for a chip's or a line's name and consumer, NUL included. */
#define GPIO_MAX_NAME_SIZE 32

/** @brief A chip (`GPIO_GET_CHIPINFO_IOCTL`). */
struct gpiochip_info {
                char     name[GPIO_MAX_NAME_SIZE];  /**< `gpiochip0`. */
                char     label[GPIO_MAX_NAME_SIZE]; /**< The chip: `ESP32-P4`. */
                uint32_t lines;                     /**< Its lines: the pads, by GPIO number. */
};

/** @brief The most lines a request takes, and attributes a line has. */
#define GPIO_V2_LINES_MAX 64
#define GPIO_V2_LINE_NUM_ATTRS_MAX 10

/** @brief A line's flags (`gpio_v2_line_info::flags`). */
enum gpio_v2_line_flag {
        GPIO_V2_LINE_FLAG_USED = 1ULL << 0,
        GPIO_V2_LINE_FLAG_ACTIVE_LOW = 1ULL << 1,
        GPIO_V2_LINE_FLAG_INPUT = 1ULL << 2,
        GPIO_V2_LINE_FLAG_OUTPUT = 1ULL << 3,
        GPIO_V2_LINE_FLAG_EDGE_RISING = 1ULL << 4,
        GPIO_V2_LINE_FLAG_EDGE_FALLING = 1ULL << 5,
        GPIO_V2_LINE_FLAG_OPEN_DRAIN = 1ULL << 6,
        GPIO_V2_LINE_FLAG_OPEN_SOURCE = 1ULL << 7,
        GPIO_V2_LINE_FLAG_BIAS_PULL_UP = 1ULL << 8,
        GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN = 1ULL << 9,
        GPIO_V2_LINE_FLAG_BIAS_DISABLED = 1ULL << 10,
        GPIO_V2_LINE_FLAG_EVENT_CLOCK_REALTIME = 1ULL << 11,
        GPIO_V2_LINE_FLAG_EVENT_CLOCK_HTE = 1ULL << 12,
};

/** @brief Values of lines (`GPIO_V2_LINE_GET_VALUES_IOCTL`, for reference: not provided yet). */
struct gpio_v2_line_values {
                uint64_t bits;
                uint64_t mask;
};

/** @brief What an attribute is (`gpio_v2_line_attribute::id`). */
enum gpio_v2_line_attr_id {
        GPIO_V2_LINE_ATTR_ID_FLAGS = 1,
        GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES = 2,
        GPIO_V2_LINE_ATTR_ID_DEBOUNCE = 3,
};

/** @brief A line's attribute. */
struct gpio_v2_line_attribute {
                uint32_t id;      /**< `GPIO_V2_LINE_ATTR_ID_...`. */
                uint32_t padding; /**< Zero. */
                union {
                                uint64_t flags;
                                uint64_t values;
                                uint32_t debounce_period_us;
                };
};

/** @brief A line (`GPIO_V2_GET_LINEINFO_IOCTL`: `offset` set, the rest zero). */
struct gpio_v2_line_info {
                char                          name[GPIO_MAX_NAME_SIZE];     /**< The board's name for it. */
                char                          consumer[GPIO_MAX_NAME_SIZE]; /**< Who holds it, or "". */
                uint32_t                      offset;                       /**< The line: its GPIO number. */
                uint32_t                      num_attrs;                    /**< Attributes in `attrs`. */
                uint64_t                      flags;                        /**< `GPIO_V2_LINE_FLAG_...`. */
                struct gpio_v2_line_attribute attrs[GPIO_V2_LINE_NUM_ATTRS_MAX];
                uint32_t                      padding[4]; /**< Zero. */
};

/** @brief A line's flags in the first interface (`gpioline_info::flags`). */
#define GPIOLINE_FLAG_KERNEL (1UL << 0)
#define GPIOLINE_FLAG_IS_OUT (1UL << 1)
#define GPIOLINE_FLAG_ACTIVE_LOW (1UL << 2)
#define GPIOLINE_FLAG_OPEN_DRAIN (1UL << 3)
#define GPIOLINE_FLAG_OPEN_SOURCE (1UL << 4)
#define GPIOLINE_FLAG_BIAS_PULL_UP (1UL << 5)
#define GPIOLINE_FLAG_BIAS_PULL_DOWN (1UL << 6)
#define GPIOLINE_FLAG_BIAS_DISABLE (1UL << 7)

/** @brief A line in the first interface (`GPIO_GET_LINEINFO_IOCTL`: `line_offset` set). */
struct gpioline_info {
                uint32_t line_offset;
                uint32_t flags; /**< `GPIOLINE_FLAG_...`. */
                char     name[GPIO_MAX_NAME_SIZE];
                char     consumer[GPIO_MAX_NAME_SIZE];
};

/** @brief The requests' type. */
#define GPIO_IOC_MAGIC 0xB4

#define GPIO_GET_CHIPINFO_IOCTL _IOR(GPIO_IOC_MAGIC, 0x01, struct gpiochip_info)
#define GPIO_GET_LINEINFO_IOCTL _IOWR(GPIO_IOC_MAGIC, 0x02, struct gpioline_info)
#define GPIO_V2_GET_LINEINFO_IOCTL _IOWR(GPIO_IOC_MAGIC, 0x05, struct gpio_v2_line_info)

#ifdef __cplusplus
}
#endif

#endif /* BMDS_LINUX_GPIO_H */
