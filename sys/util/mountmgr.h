/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2020 Google, Inc.

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

#ifndef MOUNTMGR_H_
#define MOUNTMGR_H_

#include "../dokan.h"

#include <mountmgr.h>

// Notify a MountPoint created outside the AutoMount workflow was
// linked or the link removed to his persistante symlink.
NTSTATUS DokanSendVolumeMountPoint(__in PDokanDCB Dcb, BOOLEAN Create);

//  Query the AutoMount property state.
NTSTATUS DokanQueryAutoMount(PBOOLEAN State);

// Change the AutoMount property.
// Disabling AutoMount will make MountManage to not ask for a drive letter
// suggestion and assign the one we suggested or another one.
NTSTATUS DokanSendAutoMount(BOOLEAN State);

// Notify a new device arrived and is available to be registered.
NTSTATUS DokanSendVolumeArrivalNotification(PUNICODE_STRING DeviceName);

// Sends a control code directly to the MountManager.
NTSTATUS
DokanSendIoContlToMountManager(__in ULONG IoControlCode, __in PVOID InputBuffer,
                               __in ULONG Length, __out PVOID OutputBuffer,
                               __in ULONG OutputLength);

// Inform MountManager of the new MountPoint linked to the persistante volum.
VOID NotifyDirectoryMountPointCreated(__in PDokanDCB pDcb);
VOID NotifyDirectoryMountPointDeleted(__in PDokanDCB pDcb);

// Explicitly request mount manager to create a specific mount point for the
// DeviceName. It is outside the volume arrival notification workflow.
NTSTATUS
DokanSendVolumeCreatePoint(__in PDRIVER_OBJECT DriverObject,
                           __in PUNICODE_STRING DeviceName,
                           __in PUNICODE_STRING MountPoint);

// Request mount manager to delete a specific mount point attached to the
// DeviceName.
NTSTATUS
DokanSendVolumeDeletePoints(__in PUNICODE_STRING MountPoint,
                            __in PUNICODE_STRING DeviceName);

#endif  // MOUNTMGR_H_