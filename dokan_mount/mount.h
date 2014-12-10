/*

Copyright (c) 2007, 2008 Hiroki Asakawa asakaw@gmail.com

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/


#ifndef _MOUNT_H_
#define _MOUNT_H_

#include "dokanc.h"
#include "list.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _MOUNT_ENTRY {
	LIST_ENTRY		ListEntry;
	DOKAN_CONTROL	MountControl;
} MOUNT_ENTRY, *PMOUNT_ENTRY;

BOOL
DokanControlMount(
	LPCWSTR	MountPoint,
	LPCWSTR	DeivceName);

BOOL
DokanControlUnmount(
	LPCWSTR MountPoint);

#ifdef __cplusplus
}
#endif

#endif
