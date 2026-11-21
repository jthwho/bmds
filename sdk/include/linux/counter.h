/**
 * @file      counter.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Linux's counter character device as BMDS provides it (devplan/io.md §15.3): `/dev/counter0`,
 * the pulse counters `io/pcntN` sets up, with these requests, which the SDK's `ioctl()` carries
 * out on `counterdev_ctl` (`bmds/sys.h`); `read()` gives `struct counter_event` records. Written for
 * BMDS; the layouts, names and numbers are Linux's (`include/uapi/linux/counter.h`).
 *
 * Watched: a count's value (`COUNTER_COMPONENT_COUNT`, scope `COUNTER_SCOPE_COUNT`, `parent` the
 * count) or nothing (`COUNTER_COMPONENT_NONE`), on `COUNTER_EVENT_THRESHOLD` with `channel` the
 * count it happens on (as Linux's drivers number a count's events); other watches are `EINVAL`. A count's value, preset and threshold (Linux's sysfs) are `counterdev_ctl`'s.
 *
 * ABI: frozen. Append only.
 */

#ifndef BMDS_LINUX_COUNTER_H
#define BMDS_LINUX_COUNTER_H

#include <linux/ioctl.h>
#include <linux/types.h>

enum counter_component_type {
        COUNTER_COMPONENT_NONE,
        COUNTER_COMPONENT_SIGNAL,
        COUNTER_COMPONENT_COUNT,
        COUNTER_COMPONENT_FUNCTION,
        COUNTER_COMPONENT_SYNAPSE_ACTION,
        COUNTER_COMPONENT_EXTENSION,
};

enum counter_scope {
        COUNTER_SCOPE_DEVICE,
        COUNTER_SCOPE_SIGNAL,
        COUNTER_SCOPE_COUNT,
};

/** @brief What a watch reads when its event comes. */
struct counter_component {
                __u8 type;   /**< `enum counter_component_type`. */
                __u8 scope;  /**< `enum counter_scope`. */
                __u8 parent; /**< The count (or signal) it belongs to. */
                __u8 id;     /**< Which of its kind. */
};

enum counter_event_type {
        COUNTER_EVENT_OVERFLOW,
        COUNTER_EVENT_UNDERFLOW,
        COUNTER_EVENT_OVERFLOW_UNDERFLOW,
        COUNTER_EVENT_THRESHOLD,
        COUNTER_EVENT_INDEX,
        COUNTER_EVENT_CHANGE_OF_STATE,
        COUNTER_EVENT_CAPTURE,
        COUNTER_EVENT_DIRECTION_CHANGE,
};

/** @brief A component watched for an event. */
struct counter_watch {
                struct counter_component component;
                __u8                     event;   /**< `enum counter_event_type`. */
                __u8                     channel; /**< Where the event happens: the count. */
};

#define COUNTER_ADD_WATCH_IOCTL _IOW(0x3E, 0x00, struct counter_watch)
#define COUNTER_ENABLE_EVENTS_IOCTL _IO(0x3E, 0x01)
#define COUNTER_DISABLE_EVENTS_IOCTL _IO(0x3E, 0x02)

/** @brief An event, as `read()` gives them. */
struct counter_event {
                __u64                timestamp __attribute__((aligned(8))); /**< Monotonic ns. */
                __u64                value __attribute__((aligned(8)));     /**< The component's value then. */
                struct counter_watch watch;
                __u8                 status; /**< 0, or the errno reading the value failed with. */
};

enum counter_count_direction {
        COUNTER_COUNT_DIRECTION_FORWARD,
        COUNTER_COUNT_DIRECTION_BACKWARD,
};

enum counter_count_mode {
        COUNTER_COUNT_MODE_NORMAL,
        COUNTER_COUNT_MODE_RANGE_LIMIT,
        COUNTER_COUNT_MODE_NON_RECYCLE,
        COUNTER_COUNT_MODE_MODULO_N,
        COUNTER_COUNT_MODE_INTERRUPT_ON_TERMINAL_COUNT,
        COUNTER_COUNT_MODE_HARDWARE_RETRIGGERABLE_ONESHOT,
        COUNTER_COUNT_MODE_RATE_GENERATOR,
        COUNTER_COUNT_MODE_SQUARE_WAVE_MODE,
        COUNTER_COUNT_MODE_SOFTWARE_TRIGGERED_STROBE,
        COUNTER_COUNT_MODE_HARDWARE_TRIGGERED_STROBE,
};

enum counter_function {
        COUNTER_FUNCTION_INCREASE,
        COUNTER_FUNCTION_DECREASE,
        COUNTER_FUNCTION_PULSE_DIRECTION,
        COUNTER_FUNCTION_QUADRATURE_X1_A,
        COUNTER_FUNCTION_QUADRATURE_X1_B,
        COUNTER_FUNCTION_QUADRATURE_X2_A,
        COUNTER_FUNCTION_QUADRATURE_X2_B,
        COUNTER_FUNCTION_QUADRATURE_X4,
};

enum counter_signal_level {
        COUNTER_SIGNAL_LEVEL_LOW,
        COUNTER_SIGNAL_LEVEL_HIGH,
};

enum counter_synapse_action {
        COUNTER_SYNAPSE_ACTION_NONE,
        COUNTER_SYNAPSE_ACTION_RISING_EDGE,
        COUNTER_SYNAPSE_ACTION_FALLING_EDGE,
        COUNTER_SYNAPSE_ACTION_BOTH_EDGES,
};

enum counter_signal_polarity {
        COUNTER_SIGNAL_POLARITY_POSITIVE,
        COUNTER_SIGNAL_POLARITY_NEGATIVE,
};

#endif /* BMDS_LINUX_COUNTER_H */
