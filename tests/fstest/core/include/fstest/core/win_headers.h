// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

/// \file win_headers.h
/// \brief Central Windows SDK header wrapper with conflict-free `<ntstatus.h>`
/// inclusion and global `NTSTATUS` typedef.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

// Suppress duplicate STATUS_* macro redefinition warnings when <windows.h> was
// already included by a host translation unit prior to this header.
#ifndef _WINDOWS_
#ifndef WIN32_NO_STATUS
#define WIN32_NO_STATUS
#define FSTEST_DEFINED_WIN32_NO_STATUS_INTERNAL_
#endif
#include <windows.h>
#ifdef FSTEST_DEFINED_WIN32_NO_STATUS_INTERNAL_
#undef WIN32_NO_STATUS
#undef FSTEST_DEFINED_WIN32_NO_STATUS_INTERNAL_
#endif
#include <ntstatus.h>
#else
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4005)
#endif
#include <ntstatus.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
#endif

#include <accctrl.h>
#include <aclapi.h>
#include <sddl.h>
#include <winioctl.h>

// <windows.h> with WIN32_LEAN_AND_MEAN does not always define NTSTATUS,
// whereas <ntstatus.h> expands STATUS_* macros using (NTSTATUS). Redeclaring
// an identical typedef in global scope is valid in C++20 even if <winternl.h>,
// <ntdef.h>, or <bcrypt.h> already declared it.
typedef LONG NTSTATUS;
