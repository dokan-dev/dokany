/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2020 - 2025 Google, Inc.
  Copyright (C) 2015 - 2019 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
  Copyright (C) 2007 - 2011 Hiroki Asakawa <info@dokan-dev.net>

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

// Transport talking directly to the Dokan driver control device, so that
// dokannp3.dll only depends on KERNEL32.dll.

#ifndef DOKAN_NP_TRANSPORT_LIVE_H_
#define DOKAN_NP_TRANSPORT_LIVE_H_

#include "../dokan/dokan.h"

#include "np_mount_table.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const DOKAN_NP_TRANSPORT g_DokanNpLiveTransport;

// Converts the driver mount point list into a mount table, keeping the entries
// visible from CurrentSessionId (global mounts and mounts of that session).
// Returns ERROR_SUCCESS, ERROR_NOT_ENOUGH_MEMORY or ERROR_INVALID_PARAMETER.
DWORD DokanNpTableFromMountPointInfo(const DOKAN_MOUNT_POINT_INFO *Entries,
                                     ULONG EntryCount, ULONG CurrentSessionId,
                                     DOKAN_NP_MOUNT_TABLE **Table);

#ifdef __cplusplus
}
#endif

#endif // DOKAN_NP_TRANSPORT_LIVE_H_
