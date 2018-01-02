/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2015 - 2018 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
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

#ifndef LIST_H_
#define LIST_H_

#include <windows.h>

#ifdef _MSC_VER
#if _MSC_VER < 1300
#define FORCEINLINE __inline
#endif
#endif

FORCEINLINE
VOID InitializeListHead(PLIST_ENTRY ListHead) {
  ListHead->Flink = ListHead->Blink = ListHead;
}

FORCEINLINE
BOOLEAN
IsListEmpty(const LIST_ENTRY *ListHead) {
  return (BOOLEAN)(ListHead == NULL || ListHead->Flink == ListHead);
}

FORCEINLINE
BOOLEAN
RemoveEntryList(PLIST_ENTRY Entry) {
  PLIST_ENTRY Blink;
  PLIST_ENTRY Flink;

  if (Entry != NULL) {
    Flink = Entry->Flink;
    Blink = Entry->Blink;
    Blink->Flink = Flink;
    Flink->Blink = Blink;
    return (BOOLEAN)(Flink == Blink);
  }
  /* Assumes the list is empty */
  return TRUE;
}

FORCEINLINE
PLIST_ENTRY
RemoveHeadList(PLIST_ENTRY ListHead) {
  PLIST_ENTRY Flink;
  PLIST_ENTRY Entry;

  Entry = ListHead->Flink;
  Flink = Entry->Flink;
  ListHead->Flink = Flink;
  Flink->Blink = ListHead;
  return Entry;
}

FORCEINLINE
PLIST_ENTRY
RemoveTailList(PLIST_ENTRY ListHead) {
  PLIST_ENTRY Blink;
  PLIST_ENTRY Entry;

  Entry = ListHead->Blink;
  Blink = Entry->Blink;
  ListHead->Blink = Blink;
  Blink->Flink = ListHead;
  return Entry;
}

FORCEINLINE
VOID InsertTailList(PLIST_ENTRY ListHead, PLIST_ENTRY Entry) {
  PLIST_ENTRY Blink;

  Blink = ListHead->Blink;
  Entry->Flink = ListHead;
  Entry->Blink = Blink;
  Blink->Flink = Entry;
  ListHead->Blink = Entry;
}

FORCEINLINE
VOID InsertHeadList(PLIST_ENTRY ListHead, PLIST_ENTRY Entry) {
  PLIST_ENTRY Flink;

  Flink = ListHead->Flink;
  Entry->Flink = Flink;
  Entry->Blink = ListHead;
  Flink->Blink = Entry;
  ListHead->Flink = Entry;
}

FORCEINLINE
VOID AppendTailList(PLIST_ENTRY ListHead, PLIST_ENTRY ListToAppend) {
  PLIST_ENTRY ListEnd = ListHead->Blink;

  ListHead->Blink->Flink = ListToAppend;
  ListHead->Blink = ListToAppend->Blink;
  ListToAppend->Blink->Flink = ListHead;
  ListToAppend->Blink = ListEnd;
}

FORCEINLINE
PSINGLE_LIST_ENTRY
PopEntryList(PSINGLE_LIST_ENTRY ListHead) {
  PSINGLE_LIST_ENTRY FirstEntry;
  FirstEntry = ListHead->Next;
  if (FirstEntry != NULL) {
    ListHead->Next = FirstEntry->Next;
  }

  return FirstEntry;
}

FORCEINLINE
VOID PushEntryList(PSINGLE_LIST_ENTRY ListHead, PSINGLE_LIST_ENTRY Entry) {
  Entry->Next = ListHead->Next;
  ListHead->Next = Entry;
}

#endif // LIST_H_
