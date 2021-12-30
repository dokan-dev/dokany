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

#ifndef DOKAN_VECTOR_H_
#define DOKAN_VECTOR_H_

typedef struct _DOKAN_VECTOR {
  PVOID Items;
  size_t ItemCount;
  size_t ItemSize;
  size_t MaxItems;
  BOOL IsStackAllocated;
} DOKAN_VECTOR, *PDOKAN_VECTOR;

// Creates a new instance of DOKAN_VECTOR with default values.
DOKAN_VECTOR *DokanVector_Alloc(size_t ItemSize);

// Creates a new instance of DOKAN_VECTOR with default values.
DOKAN_VECTOR *DokanVector_AllocWithCapacity(size_t ItemSize, size_t MaxItems);

// Releases the memory associated with a DOKAN_VECTOR;
VOID DokanVector_Free(PDOKAN_VECTOR Vector);

// Appends an item to the vector.
BOOL DokanVector_PushBack(PDOKAN_VECTOR Vector, PVOID Item);

// Appends an array of items to the vector.
BOOL DokanVector_PushBackArray(PDOKAN_VECTOR Vector, PVOID Items, size_t Count);

// Removes an item from the end of the vector.
VOID DokanVector_PopBack(PDOKAN_VECTOR Vector);

// Removes multiple items from the end of the vector.
VOID DokanVector_PopBackArray(PDOKAN_VECTOR Vector, size_t Count);

// Clears all items in the vector.
VOID DokanVector_Clear(PDOKAN_VECTOR Vector);

// Retrieves the item at the specified index
PVOID DokanVector_GetItem(PDOKAN_VECTOR Vector, size_t Index);

// Retrieves the last item the vector.
PVOID DokanVector_GetLastItem(PDOKAN_VECTOR Vector);

// Retrieves the number of items in the vector.
size_t DokanVector_GetCount(PDOKAN_VECTOR Vector);

// Retrieves the current capacity of the vector.
size_t DokanVector_GetCapacity(PDOKAN_VECTOR Vector);

// Retrieves the size of items within the vector.
size_t DokanVector_GetItemSize(PDOKAN_VECTOR Vector);

#endif
