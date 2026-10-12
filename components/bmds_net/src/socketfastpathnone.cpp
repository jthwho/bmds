/**
 * @file      socketfastpathnone.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * No fast path (the host tests: there is no Ethernet driver): a socket asked
 * for one answers NotSupported, and its datagrams go through lwIP.
 */

#include "socketfastpath.h"

BMDS_NAMESPACE_BEGIN

Result<SocketFastPath *> SocketFastPath::open(const Config &config) {
        (void)config;
        return makeError<SocketFastPath *>(Error::NotSupported);
}

BMDS_NAMESPACE_END
