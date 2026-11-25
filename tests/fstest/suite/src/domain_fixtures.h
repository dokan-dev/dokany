// SPDX-License-Identifier: MIT

#pragma once

#include "fstest/suite/test_fixture.h"

namespace fstest {

// Canonical domain test fixture classes (one per NTFS conformance domain).
// Defining every fixture class once in this header avoids One Definition Rule
// (ODR) hazards across multi-file split domains (CreateOpenTest, DataIoTest,
// InfoQueriesTest, StreamsTest) and keeps all 12 domain suites uniform.
class CreateOpenTest : public NtfsTestBase {};
class DeleteTest : public NtfsTestBase {};
class DirEnumTest : public NtfsTestBase {};
class RenameMoveTest : public NtfsTestBase {};
class DataIoTest : public NtfsTestBase {};
class LocksOplocksTest : public NtfsTestBase {};
class TimestampsAttrsTest : public NtfsTestBase {};
class StreamsTest : public NtfsTestBase {};
class SecurityAclTest : public NtfsTestBase {};
class LinksReparseTest : public NtfsTestBase {};
class NamingPathsTest : public NtfsTestBase {};
class InfoQueriesTest : public NtfsTestBase {};

} // namespace fstest
