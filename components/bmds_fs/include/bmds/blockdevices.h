/**
 * @file      blockdevices.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <bmds/blockdevice.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The registry of block devices, by name: what `/dev` lists.
 *
 * Names carry numbers, even when there is one of a kind: drivers register
 * with a prefix and get the lowest free number (`sd0`, `usb0`); partitions
 * are registered by `Partitions::attach()` as `<parent>p<N>` (`sd0p0`).
 * Names are lowercase letters and digits, starting with a letter.
 *
 * Call `Vfs::init()` first.
 *
 * @par Thread Safety
 * Thread-safe (the storage lock).
 */
namespace BlockDevices {

        /** @brief Longest device name. */
        inline constexpr size_t MaxName = 11;

        /** @brief Most devices registered at once (whole devices and partitions). */
        inline constexpr size_t MaxDevices = 16;

        /** @brief A registered device. */
        struct Entry {
                        char         name[MaxName + 1] = {}; ///< Its name.
                        BlockDevice *device = nullptr;       ///< The device.
                        BlockDevice *parent = nullptr;       ///< For a partition, the whole device; else null.
        };

        /**
         * @brief Registers @p device as `<prefix><N>`, N the lowest free number.
         * @param name Receives the name.
         * @return `Ok`, `Invalid` (a bad prefix), `Exists` (already registered), `Full`.
         */
        Error addNumbered(BlockDevice &device, StringView prefix, char (&name)[MaxName + 1]);

        /**
         * @brief Registers @p device under @p name, as a partition of @p parent if not null.
         * @return `Ok`, `Invalid`, `Exists` (the name or the device), `Full`.
         */
        Error add(BlockDevice &device, StringView name, BlockDevice *parent);

        /** @brief Unregisters @p device (nothing if it isn't registered). */
        void remove(BlockDevice &device);

        /** @brief Returns the device named @p name, or null. */
        BlockDevice *find(StringView name);

        /** @brief Copies @p device's name; `NotExist` if it isn't registered. */
        Error nameOf(const BlockDevice &device, char (&name)[MaxName + 1]);

        /** @brief Copies up to `out.size()` entries, in registration order; returns how many. */
        size_t list(Span<Entry> out);

} // namespace BlockDevices

BMDS_NAMESPACE_END
