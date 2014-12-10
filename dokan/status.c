/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2008 Hiroki Asakawa info@dokan-dev.net

  http://dokan-dev.net/en

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


#include <windows.h>
#include "dokani.h"
#include "fileinfo.h"


ULONG GetNTStatus(DWORD ErrorCode)
{
	switch (ErrorCode) {
	case ERROR_DIR_NOT_EMPTY:
		return STATUS_DIRECTORY_NOT_EMPTY;
	case ERROR_ACCESS_DENIED:
		return STATUS_ACCESS_DENIED;
	case ERROR_SHARING_VIOLATION:
		return STATUS_SHARING_VIOLATION;
	case ERROR_INVALID_NAME:
		return STATUS_OBJECT_NAME_NOT_FOUND;
	case ERROR_ALREADY_EXISTS:
		return STATUS_OBJECT_NAME_COLLISION;
	case ERROR_DISK_FULL:
		return STATUS_DISK_FULL;
	default:
		return STATUS_OBJECT_NAME_NOT_FOUND;
	}
}