/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2016 Keith Newton
  Copyright (C) 2021 Google, Inc.

  http://dokan-dev.github.io

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the Free
Software Foundation; either version 3 of the License, or (at your option) any
later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef DOKAN_POOL_H_
#define DOKAN_POOL_H_

#include "dokani.h"

#define DOKAN_PULL_EVENT_TIMEOUT_MS 100
#define DOKAN_MAIN_PULL_THREAD_COUNT 4
#define BATCH_EVENT_CONTEXT_SIZE (EVENT_CONTEXT_MAX_SIZE * 4)
#define DOKAN_IO_BATCH_SIZE                                                    \
  ((SIZE_T)(FIELD_OFFSET(DOKAN_IO_BATCH, EventContext)) +                      \
   BATCH_EVENT_CONTEXT_SIZE)

PTP_POOL GetThreadPool();
int InitializePool();
VOID CleanupPool();

PDOKAN_IO_BATCH PopIoBatchBuffer();
VOID PushIoBatchBuffer(PDOKAN_IO_BATCH IoBatch);
VOID FreeIoBatchBuffer(PDOKAN_IO_BATCH IoBatch);

PDOKAN_IO_EVENT PopIoEventBuffer();
VOID PushIoEventBuffer(PDOKAN_IO_EVENT IoEvent);

PEVENT_INFORMATION PopEventResult();
VOID PushEventResult(PEVENT_INFORMATION EventResult);
VOID FreeEventResult(PEVENT_INFORMATION EventResult);

PDOKAN_OPEN_INFO PopFileOpenInfo();
VOID PushFileOpenInfo(PDOKAN_OPEN_INFO FileInfo);
VOID FreeFileOpenInfo(PDOKAN_OPEN_INFO FileInfo);

PDOKAN_VECTOR PopDirectoryList();
VOID PushDirectoryList(PDOKAN_VECTOR DirectoryList);

#endif