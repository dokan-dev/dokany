#ifndef DOKAN_VECTOR_H_
#define DOKAN_VECTOR_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _DOKAN_VECTOR {
	void *Items;
	size_t ItemCount;
	size_t ItemSize;
	size_t MaxItems;
	BOOL IsStackAllocated;
} DOKAN_VECTOR, *PDOKAN_VECTOR;

// Creates a new instance of DOKAN_VECTOR with default values.
DOKAN_API DOKAN_VECTOR* DOKAN_CALLBACK DokanVector_Alloc(size_t ItemSize);

// Creates a new instance of DOKAN_VECTOR with default values.
DOKAN_API DOKAN_VECTOR* DOKAN_CALLBACK DokanVector_AllocWithCapacity(size_t ItemSize, size_t MaxItems);

// Creates a new instance of DOKAN_VECTOR with default values on the stack.
BOOL DOKANAPI DokanVector_StackAlloc(DOKAN_VECTOR *Vector, size_t ItemSize);

// Creates a new instance of DOKAN_VECTOR with default values on the stack.
BOOL DOKANAPI DokanVector_StackAllocWithCapacity(DOKAN_VECTOR *Vector, size_t ItemSize, size_t MaxItems);

// Releases the memory associated with a DOKAN_VECTOR;
void DOKANAPI DokanVector_Free(DOKAN_VECTOR *Vector);

// Appends an item to the vector.
BOOL DOKANAPI DokanVector_PushBack(DOKAN_VECTOR *Vector, void *Item);

// Appends an array of items to the vector.
BOOL DOKANAPI DokanVector_PushBackArray(DOKAN_VECTOR *Vector, void *Items, size_t Count);

// Removes an item from the end of the vector.
void DOKANAPI DokanVector_PopBack(DOKAN_VECTOR *Vector);

// Removes multiple items from the end of the vector.
void DOKANAPI DokanVector_PopBackArray(DOKAN_VECTOR *Vector, size_t Count);

// Clears all items in the vector.
void DOKANAPI DokanVector_Clear(DOKAN_VECTOR *Vector);

// Retrieves the item at the specified index
DOKAN_API void* DOKAN_CALLBACK DokanVector_GetItem(DOKAN_VECTOR *Vector, size_t Index);

// Retrieves the last item the vector.
DOKAN_API void* DOKAN_CALLBACK DokanVector_GetLastItem(DOKAN_VECTOR *Vector);

// Retrieves the number of items in the vector.
size_t DOKANAPI DokanVector_GetCount(DOKAN_VECTOR *Vector);

// Retrieves the current capacity of the vector.
size_t DOKANAPI DokanVector_GetCapacity(DOKAN_VECTOR *Vector);

// Retrieves the size of items within the vector.
size_t DOKANAPI DokanVector_GetItemSize(DOKAN_VECTOR *Vector);

#ifdef __cplusplus
}
#endif

#endif
