/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2015 - 2016 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
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

#include <stdio.h>
#include "dokani.h"

ULONG DOKANAPI DokanVersion() { return DOKAN_VERSION; }

ULONG DOKANAPI DokanDriverVersion() {
  ULONG version = 0;
  ULONG ret = 0;

  if (SendToDevice(DOKAN_GLOBAL_DEVICE_NAME, IOCTL_TEST,
                   NULL,          // InputBuffer
                   0,             // InputLength
                   &version,      // OutputBuffer
                   sizeof(ULONG), // OutputLength
                   &ret)) {

    return version;
  }

  return STATUS_SUCCESS;
}