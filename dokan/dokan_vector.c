#include "dokani.h"
#include "dokan_vector.h"

#include <assert.h>

#define DEFAULT_ITEM_COUNT 128

// Creates a new instance of DOKAN_VECTOR with default values.
DOKAN_VECTOR* DokanVector_Alloc(size_t ItemSize) {

	assert(ItemSize > 0);

	if(ItemSize == 0) {
		
		DbgPrintW(L"Cannot allocate a DOKAN_VECTOR with an ItemSize of 0.\n");
		return NULL;
	}

	DOKAN_VECTOR *vector = (DOKAN_VECTOR*)malloc(sizeof(DOKAN_VECTOR));

	vector->Items = malloc(ItemSize * DEFAULT_ITEM_COUNT);
	vector->ItemCount = 0;
	vector->ItemSize = ItemSize;
	vector->MaxItems = DEFAULT_ITEM_COUNT;
	vector->IsStackAllocated = FALSE;

	return vector;
}

// Creates a new instance of DOKAN_VECTOR with default values.
DOKAN_VECTOR* DokanVector_AllocWithCapacity(size_t ItemSize, size_t MaxItems) {
	
	assert(ItemSize > 0);

	if(ItemSize == 0) {
		DbgPrintW(L"Cannot allocate a DOKAN_VECTOR with an ItemSize of 0.\n");
		return NULL;
	}

	DOKAN_VECTOR *vector = (DOKAN_VECTOR*)malloc(sizeof(DOKAN_VECTOR));

	if(MaxItems > 0) {
		
		vector->Items = malloc(ItemSize * MaxItems);
	}
	else {
		
		vector->Items = NULL;
	}

	vector->ItemCount = 0;
	vector->ItemSize = ItemSize;
	vector->MaxItems = MaxItems;
	vector->IsStackAllocated = FALSE;

	return vector;
}

// Creates a new instance of DOKAN_VECTOR with default values on the stack.
BOOL DokanVector_StackAlloc(DOKAN_VECTOR *Vector, size_t ItemSize) {

	assert(Vector && ItemSize > 0);

	if(ItemSize == 0) {
		
		DbgPrintW(L"Cannot allocate a DOKAN_VECTOR with an ItemSize of 0.\n");
		
		ZeroMemory(Vector, sizeof(DOKAN_VECTOR));

		return FALSE;
	}

	Vector->Items = malloc(ItemSize * DEFAULT_ITEM_COUNT);
	Vector->ItemCount = 0;
	Vector->ItemSize = ItemSize;
	Vector->MaxItems = DEFAULT_ITEM_COUNT;
	Vector->IsStackAllocated = TRUE;

	return TRUE;
}

// Creates a new instance of DOKAN_VECTOR with default values on the stack.
BOOL DokanVector_StackAllocWithCapacity(DOKAN_VECTOR *Vector, size_t ItemSize, size_t MaxItems) {

	assert(Vector && ItemSize > 0);

	if(ItemSize == 0) {

		DbgPrintW(L"Cannot allocate a DOKAN_VECTOR with an ItemSize of 0.\n");
		
		ZeroMemory(Vector, sizeof(DOKAN_VECTOR));

		return FALSE;
	}

	if(MaxItems > 0) {

		Vector->Items = malloc(ItemSize * MaxItems);
	}
	else {

		Vector->Items = NULL;
	}

	Vector->ItemCount = 0;
	Vector->ItemSize = ItemSize;
	Vector->MaxItems = MaxItems;
	Vector->IsStackAllocated = TRUE;

	return TRUE;
}

// Releases the memory associated with a DOKAN_VECTOR;
void DokanVector_Free(DOKAN_VECTOR *Vector) {

	if(Vector) {
		
		if(Vector->Items) {
			
			free(Vector->Items);
		}

		if(!Vector->IsStackAllocated) {
			
			free(Vector);
		}
	}
}

// Appends an item to the vector.
BOOL DokanVector_PushBack(DOKAN_VECTOR *Vector, void *Item) {
	
	assert(Vector && Item);

	if(Vector->ItemCount + 1 >= Vector->MaxItems) {
		
		if(!DokanVector_Grow(Vector, 1)) {
			
			return FALSE;
		}
	}

	memcpy_s(((BYTE*)Vector->Items) + Vector->ItemSize * Vector->ItemCount, (Vector->MaxItems - Vector->ItemCount) * Vector->ItemSize, Item, Vector->ItemSize);

	++Vector->ItemCount;

	return TRUE;
}

// Appends an array of items to the vector.
BOOL DokanVector_PushBackArray(DOKAN_VECTOR *Vector, void *Items, size_t Count) {

	assert(Vector && Items);

	if(Count == 0) {
		return TRUE;
	}

	if(Vector->ItemCount + Count >= Vector->MaxItems) {

		if(!DokanVector_Grow(Vector, Count)) {
			return FALSE;
		}
	}

	memcpy_s(((BYTE*)Vector->Items) + Vector->ItemSize * Vector->ItemCount, (Vector->MaxItems - Vector->ItemCount) * Vector->ItemSize, Items, Vector->ItemSize * Count);

	Vector->ItemCount += Count;

	return TRUE;
}

// Removes an item from the end of the vector.
void DokanVector_PopBack(DOKAN_VECTOR *Vector) {

	assert(Vector && Vector->ItemCount > 0);

	if(Vector->ItemCount > 0) {
		
		--Vector->ItemCount;
	}
}

// Removes multiple items from the end of the vector.
void DokanVector_PopBackArray(DOKAN_VECTOR *Vector, size_t Count) {

	assert(Count <= Vector->ItemCount);

	if(Count > Vector->ItemCount) {
		
		Vector->ItemCount = 0;
	}
	else {

		Vector->ItemCount -= Count;
	}
}

// Clears all items in the vector.
void DokanVector_Clear(DOKAN_VECTOR *Vector) {

	assert(Vector);

	Vector->ItemCount = 0;
}

// Retrieves the item at the specified index
void* DokanVector_GetItem(DOKAN_VECTOR *Vector, size_t Index) {

	assert(Vector && Index < Vector->ItemCount);

	if(Index < Vector->ItemCount) {
		
		return ((BYTE*)Vector->Items) + Vector->ItemSize * Index;
	}

	return NULL;
}

// Retrieves the last item the vector.
void* DokanVector_GetLastItem(DOKAN_VECTOR *Vector) {
	
	assert(Vector);

	if(Vector->ItemCount > 0) {

		return ((BYTE*)Vector->Items) + Vector->ItemSize * (Vector->ItemCount - 1);
	}

	return NULL;
}

// Increases the internal capacity of the vector.
BOOL DokanVector_Grow(DOKAN_VECTOR *Vector, size_t MinimumIncrease) {

	if(MinimumIncrease == 0 && Vector->MaxItems == 0) {
		
		Vector->Items = malloc(Vector->ItemSize * DEFAULT_ITEM_COUNT);
		Vector->MaxItems = DEFAULT_ITEM_COUNT;

		return TRUE;
	}

	size_t newSize = Vector->MaxItems * 2;

	if(newSize < DEFAULT_ITEM_COUNT) {
		
		newSize = DEFAULT_ITEM_COUNT;
	}

	if(newSize <= Vector->MaxItems + MinimumIncrease) {
		
		newSize = Vector->MaxItems + MinimumIncrease + MinimumIncrease;
	}

	void *newItems = realloc(Vector->Items, newSize * Vector->ItemSize);

	if(newItems) {
		
		Vector->Items = newItems;
		Vector->MaxItems = newSize;

		return TRUE;
	}

	return FALSE;
}

// Retrieves the number of items in the vector.
size_t DokanVector_GetCount(DOKAN_VECTOR *Vector) {

	assert(Vector);

	if(Vector) {

		return Vector->ItemCount;
	}

	return 0;
}

// Retrieves the current capacity of the vector.
size_t DokanVector_GetCapacity(DOKAN_VECTOR *Vector) {

	assert(Vector);

	if(Vector) {

		return Vector->MaxItems;
	}

	return 0;
}

// Retrieves the size of items within the vector.
size_t DokanVector_GetItemSize(DOKAN_VECTOR *Vector) {

	assert(Vector);

	if(Vector) {
		
		return Vector->ItemSize;
	}

	return 0;
}