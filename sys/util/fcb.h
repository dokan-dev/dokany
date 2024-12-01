/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2020 - 2023 Google, Inc.

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

#ifndef FCB_H_
#define FCB_H_

#include "../dokan.h"

// Dokan specific behavior filename.
extern const UNICODE_STRING g_KeepAliveFileName;
extern const UNICODE_STRING g_NotificationFileName;

// Decrements the FileCount on the given Fcb, which either deletes it or
// schedules it for garbage collection if the FileCount becomes 0.
NTSTATUS
DokanFreeFCB(__in PDokanVCB Vcb, __in PDokanFCB Fcb);

// Return the FCB instance attached to the FileName if already present in the
// VolumeControlBlock Fcb list.
PDokanFCB DokanGetFCB(__in PREQUEST_CONTEXT RequestContext,
                      __in PWCHAR FileName, __in ULONG FileNameLength,
                      __out BOOLEAN* IsAlreadyOpen);

// Starts the FCB garbage collector thread for the given volume. If the
// Vcb->FcbGarbageCollectorThread is NULL after this then it could not be
// started.
VOID DokanStartFcbGarbageCollector(PDokanVCB Vcb);

// Schedules the given FCB for garbage collection, and returns whether
// scheduling it was successful. Currently it would only fail if garbage
// collection is not enabled. It must be called with the VCB locked RW.
BOOLEAN DokanScheduleFcbForGarbageCollection(__in PDokanVCB Vcb,
                                             __in PDokanFCB Fcb);

// Cancels the scheduled garbage collection of the given FCB. This is a no-op if
// collection was never scheduled. It must be called with the VCB locked RW. The
// NewFileName must be the same as the current Fcb->FileName but may be in a
// different case. This is to prevent the case from being sticky if the user
// abandons the use of one case representation and starts using a different one
// within the GC interval. This function always deletes or transfers ownership
// of NewFileName->Buffer.
VOID DokanCancelFcbGarbageCollection(__in PDokanFCB Fcb,
                                     _Inout_ PUNICODE_STRING NewFileName);

// Forces FCB garbage collection (if enabled) and returns whether anything was
// deleted as a consequence. This must be called with the VCB locked RW.
BOOLEAN DokanForceFcbGarbageCollection(__in PDokanVCB Vcb);

// Deletes the given FCB with no questions asked. This should only be used as a
// helper by e.g. the garbage collector, and not by an I/O handling function.
// The VCB and FCB must both be locked RW when this is called. After it returns,
// do not unlock the FCB.
VOID DokanDeleteFcb(__in PDokanVCB Vcb, __in PDokanFCB Fcb,
                    __in BOOLEAN RemoveFromTable);

// Comparison callback routine for the AVL table.
// Both pointers are FCB. Return the compare result based on the FileName.
RTL_GENERIC_COMPARE_RESULTS DokanCompareFcb(__in struct _RTL_AVL_TABLE* Table,
                                            __in PVOID FirstStruct,
                                            __in PVOID SecondStruct);

// Allocation callback routine for the AVL table.
// The size requested contains the AVL node header and the data.
PVOID DokanAllocateFcbAvl(__in struct _RTL_AVL_TABLE* Table,
                          __in CLONG ByteSize);

// Deallocation callback routine for the AVL table.
VOID DokanFreeFcbAvl(__in struct _RTL_AVL_TABLE* Table, __in PVOID Buffer);

// Update the filename of the given Fcb.
// The Vcb & Fcb must be acquired priore to the call.
VOID DokanRenameFcb(__in PREQUEST_CONTEXT Request, __in PDokanFCB Fcb,
                    __in PWCH FileName, __in USHORT FileNameLength);

#endif  // FCB_H_
