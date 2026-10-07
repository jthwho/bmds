/**
 * @file      fslayer.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The storage lock every filesystem call runs under (fslayer.h).
 */

#include <bmds/staticobject.h>

#include "fatlayer.h"
#include "fslayer.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constinit StaticObject<Mutex> storageLock;

} // namespace

Mutex &Fs::lock() {
        return *storageLock;
}

Error Fs::init() {
        if (!storageLock.isCreated()) storageLock.create();
        return Fat::init();
}

BMDS_NAMESPACE_END
