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

#include "dokani.h"
#include "dokan_vector.h"

#include <assert.h>

#define DEFAULT_ITEM_COUNT 128

// Increases the internal capacity of the vector.
BOOL DokanVector_Grow(PDOKAN_VECTOR Vector, size_t MinimumIncrease);

// Creates a new instance of DOKAN_VECTOR with default values.
PDOKAN_VECTOR DokanVector_Alloc(size_t ItemSize) {
  assert(ItemSize > 0);
  if (ItemSize == 0) {
    DbgPrintW(L"Cannot allocate a DOKAN_VECTOR with an ItemSize of 0.\n");
    return NULL;
  }
  PDOKAN_VECTOR vector = (PDOKAN_VECTOR)malloc(sizeof(DOKAN_VECTOR));
  if (!vector) {
    DbgPrintW(L"DOKAN_VECTOR allocation failed.\n");
    return NULL;
  }
  vector->Items = malloc(ItemSize * DEFAULT_ITEM_COUNT);
  if (!vector->Items) {
    DbgPrintW(L"DOKAN_VECTOR Items allocation failed.\n");
    free(vector);
    return NULL;
  }
  vector->ItemCount = 0;
  vector->ItemSize = ItemSize;
  vector->MaxItems = DEFAULT_ITEM_COUNT;
  vector->IsStackAllocated = FALSE;
  return vector;
}

// Creates a new instance of DOKAN_VECTOR with default values.
PDOKAN_VECTOR DokanVector_AllocWithCapacity(size_t ItemSize, size_t MaxItems) {
  assert(ItemSize > 0);
  if (ItemSize == 0) {
    DbgPrintW(L"Cannot allocate a DOKAN_VECTOR with an ItemSize of 0.\n");
    return NULL;
  }
  PDOKAN_VECTOR vector = (PDOKAN_VECTOR)malloc(sizeof(DOKAN_VECTOR));
  if (!vector) {
    DbgPrintW(L"DOKAN_VECTOR allocation failed.\n");
    return NULL;
  }
  if (MaxItems > 0) {
    vector->Items = malloc(ItemSize * MaxItems);
    if (!vector->Items) {
      DbgPrintW(L"DOKAN_VECTOR Items allocation failed.\n");
      free(vector);
      return NULL;
    }
  } else {
    vector->Items = NULL;
  }
  vector->ItemCount = 0;
  vector->ItemSize = ItemSize;
  vector->MaxItems = MaxItems;
  vector->IsStackAllocated = FALSE;
  return vector;
}

// Releases the memory associated with a DOKAN_VECTOR;
VOID DokanVector_Free(PDOKAN_VECTOR Vector) {
  if (Vector) {
    if (Vector->Items) {
      free(Vector->Items);
    }
    if (!Vector->IsStackAllocated) {
      free(Vector);
    }
  }
}

// Appends an item to the vector.
BOOL DokanVector_PushBack(PDOKAN_VECTOR Vector, PVOID Item) {
  assert(Vector && Item);
  if (Vector->ItemCount + 1 >= Vector->MaxItems) {
    if (!DokanVector_Grow(Vector, 1)) {
      return FALSE;
    }
  }
  memcpy_s(((BYTE *)Vector->Items) + Vector->ItemSize * Vector->ItemCount,
           (Vector->MaxItems - Vector->ItemCount) * Vector->ItemSize, Item,
           Vector->ItemSize);
  ++Vector->ItemCount;
  return TRUE;
}

// Appends an array of items to the vector.
BOOL DokanVector_PushBackArray(PDOKAN_VECTOR Vector, PVOID Items,
                               size_t Count) {
  assert(Vector && Items);
  if (Count == 0) {
    return TRUE;
  }
  if (Vector->ItemCount + Count >= Vector->MaxItems) {
    if (!DokanVector_Grow(Vector, Count)) {
      return FALSE;
    }
  }
  memcpy_s(((BYTE *)Vector->Items) + Vector->ItemSize * Vector->ItemCount,
           (Vector->MaxItems - Vector->ItemCount) * Vector->ItemSize, Items,
           Vector->ItemSize * Count);
  Vector->ItemCount += Count;
  return TRUE;
}

// Removes an item from the end of the vector.
VOID DokanVector_PopBack(PDOKAN_VECTOR Vector) {
  assert(Vector && Vector->ItemCount > 0);
  if (Vector->ItemCount > 0) {
    --Vector->ItemCount;
  }
}

// Removes multiple items from the end of the vector.
VOID DokanVector_PopBackArray(PDOKAN_VECTOR Vector, size_t Count) {
  assert(Count <= Vector->ItemCount);
  if (Count > Vector->ItemCount) {
    Vector->ItemCount = 0;
  } else {
    Vector->ItemCount -= Count;
  }
}

// Clears all items in the vector.
VOID DokanVector_Clear(PDOKAN_VECTOR Vector) {
  assert(Vector);
  Vector->ItemCount = 0;
}

// Retrieves the item at the specified index
PVOID DokanVector_GetItem(PDOKAN_VECTOR Vector, size_t Index) {
  assert(Vector && Index < Vector->ItemCount);
  if (Index < Vector->ItemCount) {
    return ((BYTE *)Vector->Items) + Vector->ItemSize * Index;
  }
  return NULL;
}

// Retrieves the last item the vector.
PVOID DokanVector_GetLastItem(PDOKAN_VECTOR Vector) {
  assert(Vector);
  if (Vector->ItemCount > 0) {
    return ((BYTE *)Vector->Items) + Vector->ItemSize * (Vector->ItemCount - 1);
  }
  return NULL;
}

// Increases the internal capacity of the vector.
BOOL DokanVector_Grow(PDOKAN_VECTOR Vector, size_t MinimumIncrease) {
  if (MinimumIncrease == 0 && Vector->MaxItems == 0) {
    Vector->Items = malloc(Vector->ItemSize * DEFAULT_ITEM_COUNT);
    if (!Vector->Items) {
      DbgPrintW(L"DOKAN_VECTOR Items allocation failed.\n");
      return FALSE;
    }
    Vector->MaxItems = DEFAULT_ITEM_COUNT;
    return TRUE;
  }
  size_t newSize = Vector->MaxItems * 2;
  if (newSize < DEFAULT_ITEM_COUNT) {
    newSize = DEFAULT_ITEM_COUNT;
  }
  if (newSize <= Vector->MaxItems + MinimumIncrease) {
    newSize = Vector->MaxItems + MinimumIncrease + MinimumIncrease;
  }
  PVOID newItems = realloc(Vector->Items, newSize * Vector->ItemSize);
  if (newItems) {
    Vector->Items = newItems;
    Vector->MaxItems = newSize;
    return TRUE;
  }
  return FALSE;
}

// Retrieves the number of items in the vector.
size_t DokanVector_GetCount(PDOKAN_VECTOR Vector) {
  assert(Vector);
  if (Vector) {
    return Vector->ItemCount;
  }
  return 0;
}

// Retrieves the current capacity of the vector.
size_t DokanVector_GetCapacity(PDOKAN_VECTOR Vector) {
  assert(Vector);
  if (Vector) {
    return Vector->MaxItems;
  }
  return 0;
}

// Retrieves the size of items within the vector.
size_t DokanVector_GetItemSize(PDOKAN_VECTOR Vector) {
  assert(Vector);
  if (Vector) {
    return Vector->ItemSize;
  }
  return 0;
}