/**
 * @file      keys.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Keys as scripts name them (`Event.key`): letters and digits as themselves (small), the rest by
 * name ("Left", "Space", "F5"). angel's screens and Preso's decks (devplan/preso.md §7.10) name
 * them the same.
 */

#include <stdio.h>
#include "angel.h"

namespace angel {

        const KeyName keyNames[] = {
                {0x28, "Enter"},      {0x29, "Escape"},   {0x2a, "Backspace"}, {0x2b, "Tab"},   {0x2c, "Space"},
                {0x2d, "-"},          {0x2e, "="},        {0x2f, "["},         {0x30, "]"},     {0x31, "\\"},
                {0x33, ";"},          {0x34, "'"},        {0x35, "`"},         {0x36, ","},     {0x37, "."},
                {0x38, "/"},          {0x39, "CapsLock"}, {0x3a, "F1"},        {0x3b, "F2"},    {0x3c, "F3"},
                {0x3d, "F4"},         {0x3e, "F5"},       {0x3f, "F6"},        {0x40, "F7"},    {0x41, "F8"},
                {0x42, "F9"},         {0x43, "F10"},      {0x44, "F11"},       {0x45, "F12"},   {0x46, "PrintScreen"},
                {0x47, "ScrollLock"}, {0x48, "Pause"},    {0x49, "Insert"},    {0x4a, "Home"},  {0x4b, "PageUp"},
                {0x4c, "Delete"},     {0x4d, "End"},      {0x4e, "PageDown"},  {0x4f, "Right"}, {0x50, "Left"},
                {0x51, "Down"},       {0x52, "Up"},       {0x53, "NumLock"},   {0x58, "Enter"}, {0xe0, "Ctrl"},
                {0xe1, "Shift"},      {0xe2, "Alt"},      {0xe3, "Super"},     {0xe4, "Ctrl"},  {0xe5, "Shift"},
                {0xe6, "Alt"},        {0xe7, "Super"},
        };
        const size_t keyNameCount = sizeof(keyNames) / sizeof(keyNames[0]);

        void keyName(uint32_t usage, char *out, size_t size) {
                out[0] = 0;
                if ((usage >> 16) != 7) return;
                const uint32_t u = usage & 0xffff;
                if (u >= 0x04 && u <= 0x1d) {
                        snprintf(out, size, "%c", char('a' + (u - 0x04)));
                } else if (u >= 0x1e && u <= 0x27) {
                        snprintf(out, size, "%c", u == 0x27 ? '0' : char('1' + (u - 0x1e)));
                } else if (u >= 0x59 && u <= 0x62) {
                        snprintf(out, size, "Keypad%c", u == 0x62 ? '0' : char('1' + (u - 0x59)));
                } else {
                        for (size_t i = 0; i < keyNameCount; i++) {
                                if (keyNames[i].usage == u) {
                                        snprintf(out, size, "%s", keyNames[i].name);
                                        return;
                                }
                        }
                }
                return;
        }

} // namespace angel
