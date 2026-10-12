/**
 * @file      taskowner.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/detail/platform.h>
#include <bmds/taskowner.h>

BMDS_NAMESPACE_BEGIN

TaskOwner *TaskOwner::current() {
        return static_cast<TaskOwner *>(Detail::taskOwner());
}

TaskOwner *TaskOwner::of(void *task) {
        return static_cast<TaskOwner *>(Detail::taskOwnerOf(task));
}

void TaskOwner::setCurrent(TaskOwner *owner) {
        Detail::setTaskOwner(owner);
        return;
}

BMDS_NAMESPACE_END
