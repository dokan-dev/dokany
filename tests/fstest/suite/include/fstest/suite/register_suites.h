// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dokan Project Contributors

/// \file register_suites.h
/// \brief Linker anchors that retain every NTFS conformance domain suite in
/// static library builds.

#pragma once

namespace fstest {

/// \name Per-domain linker anchor functions
/// Defined in `suite/src/domains/*_test.cpp` so static library builds retain
/// every GoogleTest suite translation unit even without `/WHOLEARCHIVE`.
/// \{
void ForceLink_CreateOpen();
void ForceLink_CreateOpenSecurity();
void ForceLink_Delete();
void ForceLink_DirEnum();
void ForceLink_RenameMove();
void ForceLink_DataIo();
void ForceLink_DataIoSparseMmap();
void ForceLink_LocksOplocks();
void ForceLink_TimestampsAttrs();
void ForceLink_Streams();
void ForceLink_StreamsOps();
void ForceLink_SecurityAcl();
void ForceLink_LinksReparse();
void ForceLink_NamingPaths();
void ForceLink_InfoQueries();
void ForceLink_InfoQueriesNotifyEa();
/// \}

/// \brief Forces linker retention of all 12 NTFS conformance domains (16 translation
/// units) in static library builds even without `/WHOLEARCHIVE`.
void LinkAllSuites();

} // namespace fstest
