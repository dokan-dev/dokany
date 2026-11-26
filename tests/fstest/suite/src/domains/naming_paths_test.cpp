// SPDX-License-Identifier: MIT

#include "assertions.h"
#include "domain_fixtures.h"
#include "domain_helpers.h"
#include "fstest/core/file_ops.h"
#include "fstest/core/handle.h"
#include "fstest/core/nt_types.h"
#include "fstest/core/ntdll.h"
#include "fstest/core/os_info.h"
#include "fstest/core/path.h"
#include "fstest/core/security.h"
#include "fstest/core/status.h"
#include "fstest/suite/register_suites.h"
#include "fstest/suite/test_fixture.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ios>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Skips unless Win32 path parsing rewrites legacy DOS device names for the
// target root: a "\\?\Volume{GUID}\" root is already a verbatim path, so
// kernel32 never rewrites "NUL"/"CON" components beneath it.
#define REQUIRE_WIN32_DOS_DEVICE_PARSING()                                     \
  do {                                                                         \
    if (this->Root().kind == ::fstest::core::RootKind::kVolumeGuid) {          \
      this->RecordSkipCategory("volume_root");                                 \
      GTEST_SKIP() << "Requires a non-verbatim Win32 root: paths under a "     \
                      "\\\\?\\Volume{GUID}\\ root bypass DOS device name "     \
                      "parsing.";                                              \
    }                                                                          \
  } while (false)

// Skips when the volume is mounted on a directory junction. Win32
// RemoveDirectoryW, DeleteFileW and MoveFileExW open their source with
// FILE_OPEN_REPARSE_POINT, so on a junction path they act on (and are answered
// by) the hosting volume's mount-point directory instead of the mounted
// volume's root: they would not test the target and could unmount it.
#define REQUIRE_DIRECT_VOLUME_ROOT()                                           \
  do {                                                                         \
    if (this->Root().kind == ::fstest::core::RootKind::kSubdirectory ||        \
        this->Root().kind == ::fstest::core::RootKind::kUncSubdirectory) {     \
      this->RecordSkipCategory("volume_root");                                 \
      GTEST_SKIP() << "Requires a drive-letter, volume-GUID or UNC share "     \
                      "root: Win32 path-based delete/rename of a junction "    \
                      "mount point targets the hosting volume.";               \
    }                                                                          \
  } while (false)

namespace fstest {
namespace {

std::wstring RootOpenPath(const core::NormalizedRoot &root, bool use_extended) {
  std::wstring p = use_extended ? root.extended_root : root.win32_root;
  if (root.kind == core::RootKind::kUncShare && !p.empty() &&
      p.back() != L'\\') {
    p.push_back(L'\\');
  }
  return p;
}

// Opens an existing file or directory with FILE_FLAG_BACKUP_SEMANTICS (for
// directory metadata operations and SeRestorePrivilege backup intent such as
// SetFileShortNameW).
core::Win32Result<core::UniqueHandle>
OpenWithBackupSemantics(std::wstring_view path, DWORD access) {
  return core::CreateWin32File(path, access, core::kShareAll, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS);
}

bool IsMetadataTunnelingDisabledByRegistry(std::string *reason_out) {
  constexpr const wchar_t kFsRegKey[] =
      L"SYSTEM\\CurrentControlSet\\Control\\FileSystem";
  DWORD max_entries = 0;
  DWORD size = sizeof(max_entries);
  if (::RegGetValueW(HKEY_LOCAL_MACHINE, kFsRegKey, L"MaximumTunnelEntries",
                     RRF_RT_REG_DWORD, nullptr, &max_entries,
                     &size) == ERROR_SUCCESS &&
      max_entries == 0) {
    if (reason_out != nullptr) {
      *reason_out = "NTFS metadata tunneling is disabled by host policy "
                    "(MaximumTunnelEntries == 0).";
    }
    return true;
  }
  DWORD max_age_sec = 0;
  size = sizeof(max_age_sec);
  if (::RegGetValueW(HKEY_LOCAL_MACHINE, kFsRegKey,
                     L"MaximumTunnelEntryAgeInSeconds", RRF_RT_REG_DWORD,
                     nullptr, &max_age_sec, &size) == ERROR_SUCCESS &&
      max_age_sec == 0) {
    if (reason_out != nullptr) {
      *reason_out = "NTFS metadata tunneling is disabled by host policy "
                    "(MaximumTunnelEntryAgeInSeconds == 0).";
    }
    return true;
  }
  return false;
}

NTSTATUS SetCaseSensitiveFlag(HANDLE dir_handle, ULONG flags) {
  const auto &ntdll = core::Ntdll();
  if (ntdll.NtSetInformationFile == nullptr) {
    return STATUS_NOT_IMPLEMENTED;
  }
  nt::IoStatusBlock iosb{};
  nt::FileCaseSensitiveInfo cs_info{};
  cs_info.flags = flags;
  return ntdll.NtSetInformationFile(
      dir_handle, &iosb, &cs_info, sizeof(cs_info),
      nt::FileInfoClass::FileCaseSensitiveInformation);
}

core::NtResult<ULONG> QueryCaseSensitiveFlag(HANDLE dir_handle) {
  core::NtResult<ULONG> result{};
  const auto &ntdll = core::Ntdll();
  if (ntdll.NtQueryInformationFile == nullptr) {
    result.status = STATUS_NOT_IMPLEMENTED;
    return result;
  }
  nt::IoStatusBlock iosb{};
  nt::FileCaseSensitiveInfo queried{};
  queried.flags = 0xFFFFFFFFUL;
  result.status = ntdll.NtQueryInformationFile(
      dir_handle, &iosb, &queried, sizeof(queried),
      nt::FileInfoClass::FileCaseSensitiveInformation);
  if (nt::NtSuccess(result.status)) {
    result.value = queried.flags;
  }
  return result;
}

NTSTATUS SetCaseSensitiveFlagByPath(std::wstring_view dir_path, ULONG flags) {
  auto h = OpenWithBackupSemantics(
      dir_path, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE);
  EXPECT_WIN32_OK(h);
  if (!h) {
    return STATUS_UNSUCCESSFUL;
  }
  return SetCaseSensitiveFlag(h.value->get(), flags);
}

core::NtResult<ULONG> QueryCaseSensitiveFlagByPath(std::wstring_view dir_path) {
  core::NtResult<ULONG> result{};
  auto h =
      OpenWithBackupSemantics(dir_path, FILE_READ_ATTRIBUTES | SYNCHRONIZE);
  EXPECT_WIN32_OK(h);
  if (!h) {
    result.status = STATUS_UNSUCCESSFUL;
    return result;
  }
  return QueryCaseSensitiveFlag(h.value->get());
}

core::NtResult<core::UniqueHandle>
OpenRelativeCaseFile(HANDLE parent_handle, std::wstring_view rel_name,
                     ACCESS_MASK access, ULONG create_disposition,
                     ULONG obj_attrs) {
  return core::NtCreateFilePath(
      rel_name, access, FILE_ATTRIBUTE_NORMAL, core::kShareAll,
      create_disposition,
      nt::kFileNonDirectoryFile | nt::kFileSynchronousIoNonalert, parent_handle,
      /*ea_buffer=*/nullptr, /*ea_length=*/0, /*allocation_size=*/nullptr,
      obj_attrs);
}

::testing::AssertionResult
CheckCreateRelativeCaseFile(HANDLE parent_handle, std::wstring_view rel_name,
                            ULONG obj_attrs, std::string_view payload) {
  auto h = OpenRelativeCaseFile(parent_handle, rel_name,
                                FILE_GENERIC_READ | FILE_GENERIC_WRITE,
                                nt::kFileCreate, obj_attrs);
  if (!h) {
    return ::testing::AssertionFailure()
           << "Failed to create '" << core::WideToUtf8(rel_name)
           << "': " << core::FormatNtStatus(h.status);
  }
  auto wr = core::WriteAllBytes(h.value->get(), payload.data(), payload.size());
  if (!wr) {
    return ::testing::AssertionFailure()
           << "Failed to write '" << core::WideToUtf8(rel_name)
           << "': " << core::FormatWin32Error(wr.error);
  }
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult
CheckVerifyRelativeCaseFile(HANDLE parent_handle, std::wstring_view rel_name,
                            ULONG obj_attrs, std::string_view expected_body,
                            uint64_t *file_index_out) {
  auto h = OpenRelativeCaseFile(parent_handle, rel_name, FILE_GENERIC_READ,
                                nt::kFileOpen, obj_attrs);
  if (!h) {
    return ::testing::AssertionFailure()
           << "Failed to open '" << core::WideToUtf8(rel_name)
           << "': " << core::FormatNtStatus(h.status);
  }
  auto idx = core::GetFileIndex(h.value->get());
  if (!idx) {
    return ::testing::AssertionFailure()
           << "GetFileIndex failed for '" << core::WideToUtf8(rel_name)
           << "': " << core::FormatWin32Error(idx.error);
  }
  auto body = core::ReadAllBytes(h.value->get());
  if (!body) {
    return ::testing::AssertionFailure()
           << "ReadAllBytes failed for '" << core::WideToUtf8(rel_name)
           << "': " << core::FormatWin32Error(body.error);
  }
  if (*body.value != expected_body) {
    return ::testing::AssertionFailure()
           << "Content mismatch for '" << core::WideToUtf8(rel_name)
           << "': actual='" << *body.value << "', expected='" << expected_body
           << "'";
  }
  *file_index_out = *idx.value;
  return ::testing::AssertionSuccess();
}

// Opens the target volume's root directory itself. The path always ends in
// '\' and FILE_FLAG_OPEN_REPARSE_POINT is never passed, so a volume mounted on
// a directory junction is traversed into the mounted volume's root.
core::Win32Result<core::UniqueHandle>
OpenMountRoot(const core::NormalizedRoot &root, DWORD access) {
  std::wstring ext = RootOpenPath(root, /*use_extended=*/true);
  if (!ext.empty() && ext.back() != L'\\') {
    ext.push_back(L'\\');
  }
  auto h = OpenWithBackupSemantics(ext, access);
  if (!h && root.kind == core::RootKind::kUncShare) {
    h = OpenWithBackupSemantics(RootOpenPath(root, /*use_extended=*/false),
                                access);
  }
  return h;
}

// Returns the 8.3 alternate name FindFirstFileW reports for `path` (empty when
// the entry has none).
core::Win32Result<std::wstring> QueryAlternateName(const std::wstring &path) {
  core::Win32Result<std::wstring> res{};
  WIN32_FIND_DATAW data{};
  ::SetLastError(ERROR_SUCCESS);
  core::UniqueFindHandle find(::FindFirstFileW(path.c_str(), &data));
  const DWORD find_err = ::GetLastError();
  if (!find) {
    res.error = (find_err != ERROR_SUCCESS) ? find_err : ERROR_GEN_FAILURE;
    return res;
  }
  res.value = std::wstring(data.cAlternateFileName);
  return res;
}

bool IsHexDigit(wchar_t ch) noexcept {
  return (ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'F') ||
         (ch >= L'a' && ch <= L'f');
}

// True iff `name` has the checksum form NTFS switches to once "~1".."~4" are
// taken for a basis: the first two basis characters, four hexadecimal checksum
// digits, '~', one decimal digit, then the extension (e.g. "CO1A2B~1.TXT").
bool IsChecksumShortName(std::wstring_view name, std::wstring_view prefix2,
                         std::wstring_view dot_ext) noexcept {
  constexpr size_t kBaseLen = 8;
  if (prefix2.size() != 2 || name.size() != kBaseLen + dot_ext.size()) {
    return false;
  }
  if (!core::EqualsIgnoreCase(name.substr(0, 2), prefix2)) {
    return false;
  }
  for (size_t i = 2; i < 6; ++i) {
    if (!IsHexDigit(name[i])) {
      return false;
    }
  }
  if (name[6] != L'~' || name[7] < L'1' || name[7] > L'9') {
    return false;
  }
  return core::EqualsIgnoreCase(name.substr(kBaseLen), dot_ext);
}

// Formats a name as space-separated UTF-16 code units. core::WideToUtf8 returns
// an empty string for ill-formed UTF-16, so diagnostics for names holding
// unpaired surrogates must not go through it.
std::string FormatCodeUnits(std::wstring_view name) {
  std::string out;
  for (const wchar_t ch : name) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04X ", static_cast<unsigned int>(ch));
    out += buf;
  }
  return out;
}

std::string FormatCodeUnitList(const std::vector<std::wstring> &names) {
  std::string out;
  for (const auto &name : names) {
    out += "[";
    out += FormatCodeUnits(name);
    out += "] ";
  }
  return out;
}

} // namespace

// Spec: [MS-FSA] 2.1.5.1 — standard NTFS lookup is case-insensitive while
// preserving original creation casing in directory enumeration.
TEST_F(NamingPathsTest, CaseInsensitiveLookupAndCasePreservation) {
  REQUIRE_FS_CAPABILITY(FsCapability::kCasePreservedNames);

  const std::wstring sub = TestPath(L"case_sub");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  const std::wstring mixed = core::JoinPath(sub, L"MiXeD_CaSe.TxT");
  const std::wstring lower = core::JoinPath(sub, L"mixed_case.txt");
  const std::wstring upper = core::JoinPath(sub, L"MIXED_CASE.TXT");

  ASSERT_WIN32_OK(core::WriteFileContents(mixed, "case-payload"));

  auto id_mixed = core::QueryPathFileIndex(mixed);
  auto id_lower = core::QueryPathFileIndex(lower);
  auto id_upper = core::QueryPathFileIndex(upper);
  ASSERT_WIN32_OK(id_mixed);
  ASSERT_WIN32_OK(id_lower);
  ASSERT_WIN32_OK(id_upper);
  EXPECT_EQ(*id_lower.value, *id_mixed.value);
  EXPECT_EQ(*id_upper.value, *id_mixed.value);

  auto read_lower = core::ReadFileContents(lower);
  ASSERT_WIN32_OK(read_lower);
  EXPECT_EQ(*read_lower.value, "case-payload");

  auto dup = core::CreateWin32File(lower, GENERIC_WRITE, core::kShareAll,
                                   CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(dup);
  EXPECT_WIN32_ERROR_CODE(dup.error, ERROR_FILE_EXISTS);

  auto listed = core::ListDirectoryWin32(sub, L"*");
  ASSERT_WIN32_OK(listed);
  const std::vector<std::wstring> expected = {L".", L"..", L"MiXeD_CaSe.TxT"};
  EXPECT_MULTISET_EQ(*listed.value, expected);
}

// Spec: [MS-FSCC] 2.1.5 / Win32 CreateFileW & FindFirstFileW -- NTFS stores
// UTF-16 BMP filenames across CJK, Cyrillic, Latin, and Greek scripts
// losslessly across create, read, enumerate, and delete.
TEST_F(NamingPathsTest, UnicodeBmpAndNonAsciiScripts) {
  const std::wstring sub = TestPath(L"unicode_bmp");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  const std::vector<std::pair<std::wstring, std::string>> fixtures = {
      {L"\u65E5\u672C\u8A9E.txt", "cjk-data"},
      {L"\u0444\u0430\u0439\u043B.txt", "cyrillic-data"},
      {L"caf\u00E9_na\u00EFve.txt", "latin-accented-data"},
      {L"\u03B1\u03B2\u03B3.txt", "greek-data"},
  };

  std::vector<std::wstring> expected = {L".", L".."};
  for (const auto &[name, content] : fixtures) {
    const std::wstring full = core::JoinPath(sub, name);
    auto wr = core::WriteFileContents(full, content);
    ASSERT_WIN32_OK(wr);
    auto rd = core::ReadFileContents(full);
    ASSERT_WIN32_OK(rd);
    EXPECT_EQ(*rd.value, content);
    expected.push_back(name);
  }

  auto listed = core::ListDirectoryWin32(sub, L"*");
  ASSERT_WIN32_OK(listed);
  EXPECT_MULTISET_EQ(*listed.value, expected);

  for (const auto &[name, unused_content] : fixtures) {
    (void)unused_content;
    ASSERT_WIN32_TRUE(::DeleteFileW(core::JoinPath(sub, name).c_str()));
  }

  auto after = core::ListDirectoryWin32(sub, L"*");
  ASSERT_WIN32_OK(after);
  const std::vector<std::wstring> dots = {L".", L".."};
  EXPECT_MULTISET_EQ(*after.value, dots);
}

// Spec: [MS-FSCC] 2.1.5 / Win32 CreateFileW & FindFirstFileW -- NTFS stores
// supplementary-plane Unicode filenames as raw UTF-16 surrogate pairs and
// round-trips them identically in enumeration.
TEST_F(NamingPathsTest, UnicodeSurrogatePairsSupplementaryPlane) {
  const std::wstring sub = TestPath(L"unicode_surr");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  // U+1F4C4 (PAGE FACING UP) = {0xD83D, 0xDCC4}; U+10400 = {0xD801, 0xDC00}.
  const std::wstring emoji_name = L"doc_\U0001F4C4.txt";
  const std::wstring deseret_name = L"script_\U00010400.dat";
  ASSERT_EQ(emoji_name.size(), size_t{10});
  ASSERT_EQ(deseret_name.size(), size_t{13});

  const std::wstring emoji_path = core::JoinPath(sub, emoji_name);
  const std::wstring deseret_path = core::JoinPath(sub, deseret_name);
  ASSERT_WIN32_OK(core::WriteFileContents(emoji_path, "surrogate-emoji"));
  ASSERT_WIN32_OK(core::WriteFileContents(deseret_path, "surrogate-deseret"));

  auto rd1 = core::ReadFileContents(emoji_path);
  ASSERT_WIN32_OK(rd1);
  EXPECT_EQ(*rd1.value, "surrogate-emoji");

  auto rd2 = core::ReadFileContents(deseret_path);
  ASSERT_WIN32_OK(rd2);
  EXPECT_EQ(*rd2.value, "surrogate-deseret");

  auto listed = core::ListDirectoryWin32(sub, L"*");
  ASSERT_WIN32_OK(listed);
  const std::vector<std::wstring> expected = {L".", L"..", emoji_name,
                                              deseret_name};
  EXPECT_MULTISET_EQ(*listed.value, expected);

  ASSERT_WIN32_TRUE(::DeleteFileW(emoji_path.c_str()));
  ASSERT_WIN32_TRUE(::DeleteFileW(deseret_path.c_str()));
}

// Spec: [MS-FSCC] file names are sequences of 16-bit Unicode characters -- NTFS
// does not validate UTF-16 well-formedness, so unpaired (lone) surrogate code
// units are stored and enumerated verbatim and are never replaced by U+FFFD.
TEST_F(NamingPathsTest, UnpairedSurrogateCodeUnitsRoundTrip) {
  const std::wstring sub = TestPath(L"lone_surr");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  // Built with push_back: a lone surrogate cannot be written as a \u escape.
  std::wstring hi_name = L"lone_hi_";
  hi_name.push_back(static_cast<wchar_t>(0xD800));
  hi_name += L".txt";
  std::wstring lo_name = L"lone_lo_";
  lo_name.push_back(static_cast<wchar_t>(0xDC00));
  lo_name += L".txt";
  // Low surrogate followed by high surrogate: two code units, not a pair.
  std::wstring swapped_name = L"swapped_";
  swapped_name.push_back(static_cast<wchar_t>(0xDC00));
  swapped_name.push_back(static_cast<wchar_t>(0xD800));
  swapped_name += L".txt";

  const std::vector<std::pair<std::wstring, std::string>> fixtures = {
      {hi_name, "lone-high"},
      {lo_name, "lone-low"},
      {swapped_name, "swapped-order"},
  };

  std::vector<std::wstring> expected = {L".", L".."};
  for (const auto &[name, content] : fixtures) {
    const std::wstring full = core::JoinPath(sub, name);
    auto wr = core::WriteFileContents(full, content);
    ASSERT_WIN32_OK(wr) << "Create [" << FormatCodeUnits(name) << "] failed";
    auto rd = core::ReadFileContents(full);
    ASSERT_WIN32_OK(rd) << "Read [" << FormatCodeUnits(name) << "] failed";
    EXPECT_EQ(*rd.value, content);
    expected.push_back(name);
  }

  // A filesystem that round-trips names through UTF-8 would turn the lone
  // surrogate into U+FFFD; that name must not exist.
  EXPECT_TRUE(
      helpers::CheckFileNotFound(core::JoinPath(sub, L"lone_hi_\uFFFD.txt")));
  EXPECT_TRUE(
      helpers::CheckFileNotFound(core::JoinPath(sub, L"lone_lo_\uFFFD.txt")));

  auto listed = core::ListDirectoryWin32(sub, L"*");
  ASSERT_WIN32_OK(listed);
  std::vector<std::wstring> actual = *listed.value;
  std::sort(actual.begin(), actual.end());
  std::sort(expected.begin(), expected.end());
  EXPECT_TRUE(actual == expected)
      << "Enumerated UTF-16 code units: " << FormatCodeUnitList(actual)
      << "expected: " << FormatCodeUnitList(expected);

  for (const auto &[name, unused_content] : fixtures) {
    (void)unused_content;
    ASSERT_WIN32_TRUE(::DeleteFileW(core::JoinPath(sub, name).c_str()));
  }
}

// Spec: [MS-FSA] 2.1.4.4 — NTFS upcase table folds non-ASCII BMP case pairs
// (Latin, Greek, Cyrillic) identically during lookup and creation.
TEST_F(NamingPathsTest, UnicodeCaseFoldingInsensitive) {
  const std::wstring sub = TestPath(L"unicode_fold");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  const std::vector<std::pair<std::wstring, std::wstring>> pairs = {
      {L"\u00C4.txt", L"\u00E4.txt"}, // Latin A-diaeresis
      {L"\u03A9.txt", L"\u03C9.txt"}, // Greek Omega
      {L"\u0416.txt", L"\u0436.txt"}, // Cyrillic Zhe
  };

  for (const auto &[upper_name, lower_name] : pairs) {
    const std::wstring upper_path = core::JoinPath(sub, upper_name);
    const std::wstring lower_path = core::JoinPath(sub, lower_name);
    const std::string payload = "fold-" + core::WideToUtf8(upper_name);

    ASSERT_WIN32_OK(core::WriteFileContents(upper_path, payload));
    auto dup = core::CreateWin32File(lower_path, GENERIC_WRITE, core::kShareAll,
                                     CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
    EXPECT_FALSE(dup) << "Expected collision between "
                      << core::WideToUtf8(upper_name) << " and "
                      << core::WideToUtf8(lower_name);
    EXPECT_WIN32_ERROR_CODE(dup.error, ERROR_FILE_EXISTS);

    auto id_upper = core::QueryPathFileIndex(upper_path);
    auto id_lower = core::QueryPathFileIndex(lower_path);
    ASSERT_WIN32_OK(id_upper);
    ASSERT_WIN32_OK(id_lower);
    EXPECT_EQ(*id_lower.value, *id_upper.value);

    auto rd = core::ReadFileContents(lower_path);
    ASSERT_WIN32_OK(rd);
    EXPECT_EQ(*rd.value, payload);
  }
}

// Spec: [MS-FSA] & Win32 CreateFileW — NTFS $UpCase is a per-code-unit (1:1)
// uppercase table -- characters without a single-code-unit uppercase partner
// in that table do not fold: U+00DF (sharp s) is not "SS", U+0131 (dotless i)
// is not 'I', and U+0130 (dotted capital I) is not the uppercase of 'i'. Each
// pair coexists as two distinct files.
TEST_F(NamingPathsTest, UpcaseTableNonFoldingCharactersStayDistinct) {
  const std::wstring sub = TestPath(L"upcase_nonfold");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  const std::vector<std::pair<std::wstring, std::wstring>> pairs = {
      {L"stra\u00DFe.txt", L"STRASSE.TXT"},      // sharp s vs "SS"
      {L"\u0131_dotless.txt", L"I_DOTLESS.TXT"}, // dotless i vs 'I'
      {L"\u0130_dotted.txt", L"i_dotted.txt"},   // dotted capital I vs 'i'
  };

  std::vector<std::wstring> expected = {L".", L".."};
  for (const auto &[first_name, second_name] : pairs) {
    const std::wstring first_path = core::JoinPath(sub, first_name);
    const std::wstring second_path = core::JoinPath(sub, second_name);

    ASSERT_WIN32_OK(core::WriteFileContents(first_path, "first"));
    {
      auto second =
          core::CreateWin32File(second_path, GENERIC_WRITE, core::kShareAll,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
      ASSERT_WIN32_OK(second)
          << "'" << core::WideToUtf8(second_name) << "' must not collide with '"
          << core::WideToUtf8(first_name) << "'";
      ASSERT_WIN32_OK(core::WriteAllBytes(second.value->get(), "second", 6));
    }

    auto id_first = core::QueryPathFileIndex(first_path);
    auto id_second = core::QueryPathFileIndex(second_path);
    ASSERT_WIN32_OK(id_first);
    ASSERT_WIN32_OK(id_second);
    EXPECT_NE(*id_first.value, *id_second.value);

    EXPECT_TRUE(helpers::CheckFileContents(first_path, "first"));
    EXPECT_TRUE(helpers::CheckFileContents(second_path, "second"));
    expected.push_back(first_name);
    expected.push_back(second_name);
  }

  auto listed = core::ListDirectoryWin32(sub, L"*");
  ASSERT_WIN32_OK(listed);
  EXPECT_MULTISET_EQ(*listed.value, expected);
}

// Spec: [MS-FSA] 2.1.5.1 — illegal Win32 characters (<>"|?*) and ASCII control
// chars (0x01-0x1F) fail ERROR_INVALID_NAME / STATUS_OBJECT_NAME_INVALID.
TEST_F(NamingPathsTest, InvalidFilenameCharactersRejected) {
  const std::wstring forbidden_chars = L"<>\"|?*";
  for (wchar_t ch : forbidden_chars) {
    std::wstring name = L"bad_";
    name.push_back(ch);
    name.append(L"_char.txt");

    auto win_res = core::CreateWin32File(TestExtendedPath(name), GENERIC_WRITE,
                                         core::kShareAll, CREATE_NEW,
                                         FILE_ATTRIBUTE_NORMAL);
    EXPECT_FALSE(win_res) << "Char 0x" << std::hex
                          << static_cast<unsigned int>(ch);
    EXPECT_WIN32_ERROR_CODE(win_res.error, ERROR_INVALID_NAME);
  }

  for (wchar_t ctrl = 0x01; ctrl <= 0x1F; ++ctrl) {
    std::wstring name = L"ctrl_";
    name.push_back(ctrl);
    name.append(L".txt");

    auto win_res = core::CreateWin32File(TestExtendedPath(name), GENERIC_WRITE,
                                         core::kShareAll, CREATE_NEW,
                                         FILE_ATTRIBUTE_NORMAL);
    EXPECT_FALSE(win_res) << "Control char 0x" << std::hex
                          << static_cast<unsigned int>(ctrl);
    EXPECT_WIN32_ERROR_CODE(win_res.error, ERROR_INVALID_NAME);
  }

  // Verify NT native status directly for representative invalid characters.
  const std::vector<std::wstring> nt_bad_names = {
      L"nt_bad<.txt",
      std::wstring(L"nt_ctrl_") + static_cast<wchar_t>(1) + L".txt"};
  for (const auto &bad : nt_bad_names) {
    auto nt_res = core::NtCreateFilePath(
        TestVerbatimNtPath(bad), FILE_GENERIC_WRITE, FILE_ATTRIBUTE_NORMAL,
        core::kShareAll, nt::kFileCreate,
        nt::kFileNonDirectoryFile | nt::kFileSynchronousIoNonalert);
    EXPECT_FALSE(nt_res);
    EXPECT_NT_STATUS(nt_res.status, STATUS_OBJECT_NAME_INVALID);
  }
}

// Spec: Win32 CreateFileW & GetFullPathNameW / [MS-FSA] 2.1.5.1 -- standard
// Win32 paths strip trailing dots/spaces, whereas verbatim "\\?\" paths
// preserve literal trailing dots and spaces as distinct files.
TEST_F(NamingPathsTest, TrailingDotsAndSpacesWin32VsVerbatim) {
  const std::wstring sub = TestPath(L"trailing_sub");
  const std::wstring sub_ext = TestExtendedPath(L"trailing_sub");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub_ext.c_str(), nullptr));

  // Standard Win32 path strips trailing dots and spaces ("foo. " -> "foo").
  ASSERT_WIN32_OK(
      core::WriteFileContents(core::JoinPath(sub, L"foo. "), "win32-foo"));
  auto rd_foo = core::ReadFileContents(core::JoinPath(sub, L"foo"));
  ASSERT_WIN32_OK(rd_foo);
  EXPECT_EQ(*rd_foo.value, "win32-foo");

  // Extended verbatim \\?\ paths preserve literal trailing dot and space.
  const std::wstring bar_dot = core::JoinPath(sub_ext, L"bar.");
  const std::wstring baz_space = core::JoinPath(sub_ext, L"baz ");
  ASSERT_WIN32_OK(core::WriteFileContents(bar_dot, "verbatim-dot"));
  ASSERT_WIN32_OK(core::WriteFileContents(baz_space, "verbatim-space"));

  auto rd_bar = core::ReadFileContents(bar_dot);
  auto rd_baz = core::ReadFileContents(baz_space);
  ASSERT_WIN32_OK(rd_bar);
  ASSERT_WIN32_OK(rd_baz);
  EXPECT_EQ(*rd_bar.value, "verbatim-dot");
  EXPECT_EQ(*rd_baz.value, "verbatim-space");

  auto id_foo = core::QueryPathFileIndex(core::JoinPath(sub_ext, L"foo"));
  auto id_bar = core::QueryPathFileIndex(bar_dot);
  auto id_baz = core::QueryPathFileIndex(baz_space);
  ASSERT_WIN32_OK(id_foo);
  ASSERT_WIN32_OK(id_bar);
  ASSERT_WIN32_OK(id_baz);
  EXPECT_NE(*id_foo.value, *id_bar.value);
  EXPECT_NE(*id_foo.value, *id_baz.value);
  EXPECT_NE(*id_bar.value, *id_baz.value);

  auto listed = core::ListDirectoryWin32(sub_ext, L"*");
  ASSERT_WIN32_OK(listed);
  const std::vector<std::wstring> expected = {L".", L"..", L"foo", L"bar.",
                                              L"baz "};
  EXPECT_MULTISET_EQ(*listed.value, expected);

  ASSERT_WIN32_TRUE(::DeleteFileW(bar_dot.c_str()));
  ASSERT_WIN32_TRUE(::DeleteFileW(baz_space.c_str()));
  auto after = core::ListDirectoryWin32(sub_ext, L"*");
  ASSERT_WIN32_OK(after);
  const std::vector<std::wstring> remaining = {L".", L"..", L"foo"};
  EXPECT_MULTISET_EQ(*after.value, remaining);
}

// Spec: Win32 CreateDirectoryW path parsing -- before Windows 11 21H2 (build
// 22000) a path-qualified legacy DOS device name ("CON") is rewritten to the
// device namespace, so CreateDirectoryW fails with ERROR_DIRECTORY and no "CON"
// entry is ever created on the filesystem. Build 22000+ relaxed this rewrite,
// hence the upper OS bound.
TEST_F(NamingPathsTest, Win32CreateDirectoryConRejectedBefore24H2) {
  REQUIRE_LOCAL_VOLUME();
  REQUIRE_WIN32_DOS_DEVICE_PARSING();
  REQUIRE_MAX_OS_BUILD(core::kBuildWin11_21H2 - 1);

  const std::wstring sub = TestPath(L"dos_con");
  const std::wstring sub_ext = TestExtendedPath(L"dos_con");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub_ext.c_str(), nullptr));

  const std::wstring win32_con = core::JoinPath(sub, L"CON");
  EXPECT_WIN32_ERROR(::CreateDirectoryW(win32_con.c_str(), nullptr),
                     ERROR_DIRECTORY);
  EXPECT_TRUE(helpers::CheckFileNotFound(core::JoinPath(sub_ext, L"CON")));
}

// Spec: Win32 CreateFileW path parsing -- a path-qualified "NUL" resolves to
// the NUL device on every OS build, so writes are discarded and no "NUL" entry
// is created on the filesystem.
TEST_F(NamingPathsTest, Win32NulDeviceNameNeverReachesFilesystem) {
  REQUIRE_LOCAL_VOLUME();
  REQUIRE_WIN32_DOS_DEVICE_PARSING();

  const std::wstring sub = TestPath(L"dos_nul");
  const std::wstring sub_ext = TestExtendedPath(L"dos_nul");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub_ext.c_str(), nullptr));

  const std::wstring win32_nul = core::JoinPath(sub, L"NUL");
  {
    auto nul_dev = core::CreateWin32File(win32_nul, GENERIC_WRITE,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(nul_dev);
    ASSERT_WIN32_OK(core::WriteAllBytes(nul_dev.value->get(), "discard", 7));
  }

  EXPECT_TRUE(helpers::CheckFileNotFound(core::JoinPath(sub_ext, L"NUL")));
  EXPECT_TRUE(helpers::CheckDirEntries(sub_ext, {L".", L".."}));
}

// Spec: [MS-FSA] 2.1.5.1 / Win32 "\\?\" verbatim paths -- reserved DOS device
// names (NUL, AUX) are ordinary filenames to the filesystem: verbatim paths
// create, read, enumerate, and delete literal regular files with those names.
TEST_F(NamingPathsTest, VerbatimReservedDosDeviceNamesAreLiteralFiles) {
  const std::wstring sub_ext = TestExtendedPath(L"dos_dev");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub_ext.c_str(), nullptr));

  const std::wstring verb_nul = core::JoinPath(sub_ext, L"NUL");
  const std::wstring verb_aux = core::JoinPath(sub_ext, L"AUX");

  ASSERT_WIN32_OK(core::WriteFileContents(verb_nul, "real-nul-data"));
  ASSERT_WIN32_OK(core::WriteFileContents(verb_aux, "real-aux-data"));

  auto rd_nul = core::ReadFileContents(verb_nul);
  auto rd_aux = core::ReadFileContents(verb_aux);
  ASSERT_WIN32_OK(rd_nul);
  ASSERT_WIN32_OK(rd_aux);
  EXPECT_EQ(*rd_nul.value, "real-nul-data");
  EXPECT_EQ(*rd_aux.value, "real-aux-data");

  auto listed = core::ListDirectoryWin32(sub_ext, L"*");
  ASSERT_WIN32_OK(listed);
  const std::vector<std::wstring> expected = {L".", L"..", L"NUL", L"AUX"};
  EXPECT_MULTISET_EQ(*listed.value, expected);

  ASSERT_WIN32_TRUE(::DeleteFileW(verb_nul.c_str()));
  ASSERT_WIN32_TRUE(::DeleteFileW(verb_aux.c_str()));
}

// Spec: Win32 CreateFileW, CreateDirectoryW, MoveFileExW & DeleteFileW --
// extended-length "\\?\" paths exceeding MAX_PATH (260 chars) support create,
// read, enumerate, rename, and delete cleanly.
TEST_F(NamingPathsTest, ExtendedLengthLongPathsOverMaxPath) {
  std::wstring cur = TempDir().ExtendedPath();
  // Build nested dirs with <= 60-char components so total file path lands
  // safely in ~350-480 chars (well above MAX_PATH, well below 32768).
  for (int depth = 0; depth < 8 && cur.size() < 340; ++depth) {
    wchar_t prefix[16];
    std::swprintf(prefix, sizeof(prefix) / sizeof(prefix[0]), L"longdir_%02d_",
                  depth);
    std::wstring seg = prefix;
    seg.append(45, L'x');
    cur = core::JoinPath(cur, seg);
    ASSERT_WIN32_TRUE(::CreateDirectoryW(cur.c_str(), nullptr))
        << "Depth " << depth << " path len " << cur.size();
  }

  const std::wstring file_path = core::JoinPath(cur, L"deep_payload_file.dat");
  const std::wstring renamed_path =
      core::JoinPath(cur, L"deep_renamed_file.dat");
  ASSERT_GT(file_path.size(), static_cast<size_t>(MAX_PATH));
  ASSERT_LE(file_path.size(), size_t{500});

  ASSERT_WIN32_OK(
      core::WriteFileContents(file_path, "extended-long-path-content"));
  auto rd = core::ReadFileContents(file_path);
  ASSERT_WIN32_OK(rd);
  EXPECT_EQ(*rd.value, "extended-long-path-content");

  auto listed = core::ListDirectoryWin32(cur, L"*");
  ASSERT_WIN32_OK(listed);
  const std::vector<std::wstring> expected1 = {L".", L"..",
                                               L"deep_payload_file.dat"};
  EXPECT_MULTISET_EQ(*listed.value, expected1);

  ASSERT_WIN32_TRUE(::MoveFileExW(file_path.c_str(), renamed_path.c_str(), 0));
  auto rd2 = core::ReadFileContents(renamed_path);
  ASSERT_WIN32_OK(rd2);
  EXPECT_EQ(*rd2.value, "extended-long-path-content");

  auto listed2 = core::ListDirectoryWin32(cur, L"*");
  ASSERT_WIN32_OK(listed2);
  const std::vector<std::wstring> expected2 = {L".", L"..",
                                               L"deep_renamed_file.dat"};
  EXPECT_MULTISET_EQ(*listed2.value, expected2);

  ASSERT_WIN32_TRUE(::DeleteFileW(renamed_path.c_str()));
}

// Spec: [MS-FSCC] 2.5.1 — a 255-UTF-16-code-unit filename component succeeds,
// while a 256-code-unit component fails name-length validation.
TEST_F(NamingPathsTest, MaxComponentLength255And256Rejected) {
  std::wstring name255(251, L'a');
  name255 += L".txt";
  ASSERT_EQ(name255.size(), size_t{255});

  std::wstring name256(252, L'b');
  name256 += L".txt";
  ASSERT_EQ(name256.size(), size_t{256});

  const std::wstring path255 = TestExtendedPath(name255);
  const std::wstring path256 = TestExtendedPath(name256);
  ASSERT_LE(path255.size(), size_t{500});

  ASSERT_WIN32_OK(core::WriteFileContents(path255, "max-component-255"));
  auto rd = core::ReadFileContents(path255);
  ASSERT_WIN32_OK(rd);
  EXPECT_EQ(*rd.value, "max-component-255");

  auto listed = core::ListDirectoryWin32(TempDir().ExtendedPath(), L"*");
  ASSERT_WIN32_OK(listed);
  const std::vector<std::wstring> expected = {L".", L"..", name255};
  EXPECT_MULTISET_EQ(*listed.value, expected);
  ASSERT_WIN32_TRUE(::DeleteFileW(path255.c_str()));

  auto bad_win32 =
      core::CreateWin32File(path256, GENERIC_WRITE, core::kShareAll, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(bad_win32);
  EXPECT_WIN32_ERROR_CODE(bad_win32.error, ERROR_INVALID_NAME);

  auto bad_nt = core::NtCreateFilePath(
      TestVerbatimNtPath(name256), FILE_GENERIC_WRITE, FILE_ATTRIBUTE_NORMAL,
      core::kShareAll, nt::kFileCreate,
      nt::kFileNonDirectoryFile | nt::kFileSynchronousIoNonalert);
  EXPECT_FALSE(bad_nt);
  EXPECT_NT_STATUS(bad_nt.status, STATUS_OBJECT_NAME_INVALID);
}

// Spec: Win32 CreateFileW & GetFullPathNameW -- Win32 path resolution
// normalizes '.', '..', and redundant separators within a directory tree to the
// expected target FileIndex.
TEST_F(NamingPathsTest, DotAndDotDotPathTraversal) {
  const std::wstring sub = TestPath(L"trav_sub");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  const std::wstring sub_file = TestPath(L"trav_sub\\file.txt");
  const std::wstring top_file = TestPath(L"top.txt");
  ASSERT_WIN32_OK(core::WriteFileContents(sub_file, "in-sub"));
  ASSERT_WIN32_OK(core::WriteFileContents(top_file, "at-top"));

  auto sub_id = core::QueryPathFileIndex(sub_file);
  auto top_id = core::QueryPathFileIndex(top_file);
  ASSERT_WIN32_OK(sub_id);
  ASSERT_WIN32_OK(top_id);

  const std::wstring dot_path = TestPath(L"trav_sub\\.\\file.txt");
  const std::wstring dotdot_top = TestPath(L"trav_sub\\..\\top.txt");
  const std::wstring dotdot_roundtrip =
      TestPath(L"trav_sub\\..\\trav_sub\\.\\file.txt");
  const std::wstring double_slash = TestPath(L"trav_sub\\\\file.txt");

  auto id1 = core::QueryPathFileIndex(dot_path);
  auto id2 = core::QueryPathFileIndex(dotdot_top);
  auto id3 = core::QueryPathFileIndex(dotdot_roundtrip);
  auto id4 = core::QueryPathFileIndex(double_slash);
  ASSERT_WIN32_OK(id1);
  ASSERT_WIN32_OK(id2);
  ASSERT_WIN32_OK(id3);
  ASSERT_WIN32_OK(id4);

  EXPECT_EQ(*id1.value, *sub_id.value);
  EXPECT_EQ(*id2.value, *top_id.value);
  EXPECT_EQ(*id3.value, *sub_id.value);
  EXPECT_EQ(*id4.value, *sub_id.value);
}

// Spec: [MS-FSA] 2.1.5.1 — traversing beneath a regular file as a parent
// component fails ERROR_PATH_NOT_FOUND (or ERROR_INVALID_PARAMETER on rename).
TEST_F(NamingPathsTest, ParentIsFilePathErrorsAllApis) {
  const std::wstring reg_file = TestPath(L"regular_parent.txt");
  const std::wstring other_file = TestPath(L"other.txt");
  ASSERT_WIN32_OK(core::WriteFileContents(reg_file, "parent-is-a-file"));
  ASSERT_WIN32_OK(core::WriteFileContents(other_file, "other"));

  const std::wstring child = reg_file + L"\\child.txt";

  auto cf = core::CreateWin32File(child, GENERIC_WRITE, core::kShareAll,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(cf);
  EXPECT_WIN32_ERROR_CODE(cf.error, ERROR_PATH_NOT_FOUND);

  EXPECT_WIN32_ERROR(::CreateDirectoryW(child.c_str(), nullptr),
                     ERROR_PATH_NOT_FOUND);
  EXPECT_WIN32_ERROR(::DeleteFileW(child.c_str()), ERROR_PATH_NOT_FOUND);

  auto child_attrs = core::QueryWin32Attributes(child);
  EXPECT_FALSE(child_attrs);
  EXPECT_WIN32_ERROR_CODE(child_attrs.error, ERROR_PATH_NOT_FOUND);

  // On NTFS, renaming into a destination whose parent is a regular file fails
  // ERROR_INVALID_PARAMETER (from STATUS_INVALID_PARAMETER in rename target
  // parent resolution), while renaming from such a source fails
  // ERROR_PATH_NOT_FOUND.
  EXPECT_WIN32_ERROR(::MoveFileExW(other_file.c_str(), child.c_str(), 0),
                     ERROR_INVALID_PARAMETER);
  EXPECT_WIN32_ERROR(
      ::MoveFileExW(child.c_str(), TestPath(L"moved.txt").c_str(), 0),
      ERROR_PATH_NOT_FOUND);
}

// Spec: Win32 CreateFileW & GetFileAttributesW -- a volume/mount root opens as
// a directory and reports FILE_ATTRIBUTE_DIRECTORY.
TEST_F(NamingPathsTest, MountRootOpensAsDirectory) {
  REQUIRE_VOLUME_ROOT();

  const std::wstring root_ext = RootOpenPath(Root(), /*use_extended=*/true);
  const std::wstring root_win = RootOpenPath(Root(), /*use_extended=*/false);

  auto root_h =
      OpenWithBackupSemantics(root_ext, FILE_READ_ATTRIBUTES | SYNCHRONIZE);
  if (!root_h && Root().kind == core::RootKind::kUncShare) {
    root_h =
        OpenWithBackupSemantics(root_win, FILE_READ_ATTRIBUTES | SYNCHRONIZE);
  }
  ASSERT_WIN32_OK(root_h);
  root_h.value->reset();

  auto root_attrs = core::QueryWin32Attributes(root_ext);
  if (!root_attrs && Root().kind == core::RootKind::kUncShare) {
    root_attrs = core::QueryWin32Attributes(root_win);
  }
  ASSERT_WIN32_OK(root_attrs);
  EXPECT_NE(*root_attrs.value & FILE_ATTRIBUTE_DIRECTORY, 0u);
}

// Spec: Win32 DeleteFileW -- DeleteFileW opens its target as a non-directory
// file, so on a volume root it fails with ERROR_ACCESS_DENIED
// (STATUS_FILE_IS_A_DIRECTORY) and the volume stays healthy.
TEST_F(NamingPathsTest, MountRootDeleteFileRejected) {
  REQUIRE_VOLUME_ROOT();
  REQUIRE_DIRECT_VOLUME_ROOT();

  const std::wstring root_win = RootOpenPath(Root(), /*use_extended=*/false);

  ::SetLastError(ERROR_SUCCESS);
  const BOOL del_ok = ::DeleteFileW(root_win.c_str());
  const DWORD del_err = ::GetLastError();
  if (del_ok != FALSE) {
    FAIL() << "DeleteFileW unexpectedly succeeded on the volume root '"
           << core::WideToUtf8(root_win)
           << "'. The root cannot be restored; aborting this test.";
  }
  EXPECT_WIN32_ERROR_CODE(del_err, ERROR_ACCESS_DENIED);

  const std::wstring sentinel = TestPath(L"root_intact.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(sentinel, "still-healthy"));
  EXPECT_TRUE(helpers::CheckFileContents(sentinel, "still-healthy"));
}

// Spec: FileDispositionInformation -- a volume's root directory can never be
// marked for deletion: on a non-empty volume root (guaranteed here because
// SetUp created the isolated test directory), NTFS rejects the request with
// STATUS_DIRECTORY_NOT_EMPTY.
TEST_F(NamingPathsTest, MountRootDeleteDispositionRejected) {
  REQUIRE_VOLUME_ROOT();
  // Standard users have no DELETE right on an NTFS volume root, so the open
  // below would fail with ERROR_ACCESS_DENIED before reaching the contract.
  REQUIRE_ADMIN();

  auto del_h =
      OpenMountRoot(Root(), DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE);
  ASSERT_WIN32_OK(del_h) << "Opening the volume root with DELETE access failed";

  const auto disp =
      core::SetDisposition(del_h.value->get(), /*delete_file=*/true);
  if (disp.ok()) {
    // Roll back before the handle can close, otherwise the close commits the
    // delete of the volume root.
    const auto undo =
        core::SetDisposition(del_h.value->get(), /*delete_file=*/false);
    if (!undo.ok()) {
      // Deliberate leak: closing the handle now would delete the root, so keep
      // it open for the rest of the process.
      (void)del_h.value->release();
      FAIL() << "Delete disposition was accepted on the volume root and could "
                "not be rolled back ("
             << core::FormatNtStatus(undo.status)
             << "); the root handle was leaked to keep the volume alive.";
    }
    del_h.value->reset();
    FAIL() << "Delete disposition was accepted on the volume root (expected "
              "STATUS_DIRECTORY_NOT_EMPTY); it was rolled back.";
  }
  EXPECT_NT_STATUS(disp.status, STATUS_DIRECTORY_NOT_EMPTY);
  del_h.value->reset();

  const std::wstring sentinel = TestPath(L"root_intact.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(sentinel, "still-healthy"));
  EXPECT_TRUE(helpers::CheckFileContents(sentinel, "still-healthy"));
}

// Spec: Win32 RemoveDirectoryW -- removing a non-empty volume root fails with
// ERROR_DIR_NOT_EMPTY (from STATUS_DIRECTORY_NOT_EMPTY) for elevated callers
// and ERROR_ACCESS_DENIED for standard users without root DELETE rights, and
// the volume stays healthy.
TEST_F(NamingPathsTest, MountRootRemoveDirectoryRejected) {
  REQUIRE_VOLUME_ROOT();
  REQUIRE_DIRECT_VOLUME_ROOT();
  REQUIRE_ADMIN();

  const std::wstring root_win = RootOpenPath(Root(), /*use_extended=*/false);

  ::SetLastError(ERROR_SUCCESS);
  const BOOL rmdir_ok = ::RemoveDirectoryW(root_win.c_str());
  const DWORD rmdir_err = ::GetLastError();
  if (rmdir_ok != FALSE) {
    FAIL() << "RemoveDirectoryW unexpectedly succeeded on the volume root '"
           << core::WideToUtf8(root_win)
           << "'. A removed root cannot be restored; aborting this test.";
  }
  EXPECT_WIN32_ERROR_CODE(rmdir_err, ERROR_DIR_NOT_EMPTY);

  const std::wstring sentinel = TestPath(L"root_intact.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(sentinel, "still-healthy"));
  EXPECT_TRUE(helpers::CheckFileContents(sentinel, "still-healthy"));
}

// Spec: FileRenameInformation / Win32 MoveFileExW -- a volume's root directory
// has no parent link and cannot be renamed: Win32 MoveFileExW fails with
// ERROR_ACCESS_DENIED on NTFS and the volume stays healthy.
TEST_F(NamingPathsTest, MountRootRenameRejected) {
  REQUIRE_VOLUME_ROOT();
  REQUIRE_DIRECT_VOLUME_ROOT();
  // Standard users have no DELETE right on an NTFS volume root, so MoveFileExW
  // would fail with ERROR_ACCESS_DENIED before reaching the rename.
  REQUIRE_ADMIN();

  const std::wstring root_win = RootOpenPath(Root(), /*use_extended=*/false);
  const std::wstring moved = TestPath(L"moved_root");

  ::SetLastError(ERROR_SUCCESS);
  const BOOL move_ok = ::MoveFileExW(root_win.c_str(), moved.c_str(), 0);
  const DWORD move_err = ::GetLastError();
  if (move_ok != FALSE) {
    // Best-effort rollback so later tests still find the root in place.
    ::SetLastError(ERROR_SUCCESS);
    const BOOL restored = ::MoveFileExW(moved.c_str(), root_win.c_str(), 0);
    const DWORD restore_err = ::GetLastError();
    FAIL() << "MoveFileExW unexpectedly renamed the volume root '"
           << core::WideToUtf8(root_win) << "' to '" << core::WideToUtf8(moved)
           << "'; moving it back "
           << (restored != FALSE
                   ? std::string("succeeded")
                   : "FAILED: " + core::FormatWin32Error(restore_err));
  }
  EXPECT_WIN32_ERROR_CODE(move_err, ERROR_ACCESS_DENIED);

  const std::wstring sentinel = TestPath(L"root_intact.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(sentinel, "still-healthy"));
  EXPECT_TRUE(helpers::CheckFileContents(sentinel, "still-healthy"));
}

// Spec: [MS-FSA] 2.1.5.1 / Win32 FindFirstFileW, GetShortPathNameW &
// GetLongPathNameW -- when 8.3 short names are enabled, non-8.3 filenames
// receive a tilde alias that opens the same FileIndex and round-trips.
TEST_F(NamingPathsTest, ShortName8dot3GenerationAndLookup) {
  REQUIRE_SHORT_NAMES_ENABLED();

  const std::wstring sub = TestPath(L"short83");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  const std::wstring long_name = L"Long File Name.With.Dots.txt";
  const std::wstring long_path = core::JoinPath(sub, long_name);
  ASSERT_WIN32_OK(core::WriteFileContents(long_path, "short-name-payload"));

  auto long_id = core::QueryPathFileIndex(long_path);
  ASSERT_WIN32_OK(long_id);

  WIN32_FIND_DATAW data{};
  ::SetLastError(ERROR_SUCCESS);
  core::UniqueFindHandle find(::FindFirstFileW(long_path.c_str(), &data));
  const DWORD find_err = ::GetLastError();
  ASSERT_TRUE(find) << core::FormatWin32Error(find_err);
  const std::wstring short_name(data.cAlternateFileName);
  ASSERT_FALSE(short_name.empty())
      << "Expected non-empty 8.3 cAlternateFileName";
  EXPECT_NE(short_name.find(L'~'), std::wstring::npos);

  const std::wstring short_path = core::JoinPath(sub, short_name);
  auto short_id = core::QueryPathFileIndex(short_path);
  ASSERT_WIN32_OK(short_id);
  EXPECT_EQ(*short_id.value, *long_id.value);

  EXPECT_TRUE(helpers::CheckFileContents(short_path, "short-name-payload"));

  // GetShortPathNameW / GetLongPathNameW round-trip.
  ::SetLastError(ERROR_SUCCESS);
  const DWORD short_needed = ::GetShortPathNameW(long_path.c_str(), nullptr, 0);
  const DWORD short_err = ::GetLastError();
  ASSERT_GT(short_needed, 0u) << core::FormatWin32Error(short_err);
  std::wstring short_buf(short_needed, L'\0');
  const DWORD short_len =
      ::GetShortPathNameW(long_path.c_str(), short_buf.data(), short_needed);
  ASSERT_GT(short_len, 0u);
  short_buf.resize(short_len);

  const size_t last_sep = short_buf.find_last_of(L'\\');
  ASSERT_NE(last_sep, std::wstring::npos);
  EXPECT_TRUE(core::EqualsIgnoreCase(
      std::wstring_view(short_buf).substr(last_sep + 1), short_name))
      << "GetShortPathNameW tail '"
      << core::WideToUtf8(std::wstring_view(short_buf).substr(last_sep + 1))
      << "' != '" << core::WideToUtf8(short_name) << "'";

  ::SetLastError(ERROR_SUCCESS);
  const DWORD long_needed = ::GetLongPathNameW(short_buf.c_str(), nullptr, 0);
  const DWORD long_err = ::GetLastError();
  ASSERT_GT(long_needed, 0u) << core::FormatWin32Error(long_err);
  std::wstring long_buf(long_needed, L'\0');
  const DWORD long_len =
      ::GetLongPathNameW(short_buf.c_str(), long_buf.data(), long_needed);
  ASSERT_GT(long_len, 0u);
  long_buf.resize(long_len);

  const size_t long_sep = long_buf.find_last_of(L'\\');
  ASSERT_NE(long_sep, std::wstring::npos);
  EXPECT_EQ(std::wstring_view(long_buf).substr(long_sep + 1), long_name);
}

// Spec: NTFS 8.3 short-name generation (RtlGenerate8dot3Name) -- long names
// sharing a six-character basis receive "BASIS~1".."BASIS~4" in creation
// order; from the fifth collision on, the alias switches to the first two
// basis characters plus a four-hex-digit checksum of the long name. Every
// alias is unique and opens its own file.
TEST_F(NamingPathsTest, ShortName8dot3CollisionSequence) {
  REQUIRE_SHORT_NAMES_ENABLED();

  const std::wstring sub = TestPath(L"short83_seq");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  constexpr size_t kFileCount = 6;
  constexpr size_t kTildeNumbered = 4;
  std::vector<std::wstring> short_names;
  for (size_t i = 1; i <= kFileCount; ++i) {
    const std::wstring long_path =
        core::JoinPath(sub, L"collision_seq_" + std::to_wstring(i) + L".txt");
    ASSERT_WIN32_OK(core::WriteFileContents(long_path, "collision"));

    auto alt = QueryAlternateName(long_path);
    ASSERT_WIN32_OK(alt);
    ASSERT_FALSE(alt.value->empty())
        << "File #" << i << " has no 8.3 alternate name";

    auto long_id = core::QueryPathFileIndex(long_path);
    auto short_id = core::QueryPathFileIndex(core::JoinPath(sub, *alt.value));
    ASSERT_WIN32_OK(long_id);
    ASSERT_WIN32_OK(short_id)
        << "Alias '" << core::WideToUtf8(*alt.value) << "'";
    EXPECT_EQ(*short_id.value, *long_id.value);

    short_names.push_back(*alt.value);
  }
  ASSERT_EQ(short_names.size(), kFileCount);

  for (size_t i = 0; i < kTildeNumbered; ++i) {
    const std::wstring expected = L"COLLIS~" + std::to_wstring(i + 1) + L".TXT";
    EXPECT_TRUE(core::EqualsIgnoreCase(short_names[i], expected))
        << "File #" << (i + 1) << ": expected alias '"
        << core::WideToUtf8(expected) << "', got '"
        << core::WideToUtf8(short_names[i]) << "'";
  }
  for (size_t i = kTildeNumbered; i < kFileCount; ++i) {
    EXPECT_TRUE(IsChecksumShortName(short_names[i], L"CO", L".TXT"))
        << "File #" << (i + 1)
        << ": expected checksum alias 'COxxxx~N.TXT', got '"
        << core::WideToUtf8(short_names[i]) << "'";
  }
  for (size_t i = 0; i < kFileCount; ++i) {
    for (size_t j = i + 1; j < kFileCount; ++j) {
      EXPECT_FALSE(core::EqualsIgnoreCase(short_names[i], short_names[j]))
          << "Files #" << (i + 1) << " and #" << (j + 1) << " share alias '"
          << core::WideToUtf8(short_names[i]) << "'";
    }
  }
}

// Spec: WDK FileCaseSensitiveInformation (Windows 10 1809+) -- enables
// coexisting case-differing siblings, rejects disabling while conflicts exist,
// and allows clearing once conflicts are removed.
TEST_F(NamingPathsTest, PerDirectoryCaseSensitivityFlag) {
  REQUIRE_MIN_OS_BUILD(core::kBuildWin10_1809);
  REQUIRE_LOCAL_VOLUME();
  REQUIRE_FS_CAPABILITY(FsCapability::kCaseSensitiveDir);
  REQUIRE_ADMIN();
  REQUIRE_HOST_CASE_SENSITIVE_DIR();

  const std::wstring cs_dir = TestPath(L"case_sens_dir");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(cs_dir.c_str(), nullptr));

  auto dh = OpenWithBackupSemantics(cs_dir, FILE_READ_ATTRIBUTES |
                                                FILE_WRITE_ATTRIBUTES | DELETE |
                                                SYNCHRONIZE);
  ASSERT_WIN32_OK(dh);

  ASSERT_NT_SUCCESS(
      SetCaseSensitiveFlag(dh.value->get(), nt::kFileCsFlagCaseSensitiveDir));
  auto queried = QueryCaseSensitiveFlag(dh.value->get());
  ASSERT_NT_SUCCESS(queried.status);
  EXPECT_NE(*queried.value & nt::kFileCsFlagCaseSensitiveDir, 0u);

  const std::wstring lower_path = core::JoinPath(cs_dir, L"foo.txt");
  const std::wstring upper_path = core::JoinPath(cs_dir, L"FOO.txt");
  ASSERT_WIN32_OK(core::WriteFileContents(lower_path, "lowercase-content"));
  ASSERT_WIN32_OK(core::WriteFileContents(upper_path, "uppercase-content"));

  auto id_lower = core::QueryPathFileIndex(lower_path);
  auto id_upper = core::QueryPathFileIndex(upper_path);
  ASSERT_WIN32_OK(id_lower);
  ASSERT_WIN32_OK(id_upper);
  EXPECT_NE(*id_lower.value, *id_upper.value);

  EXPECT_TRUE(helpers::CheckFileContents(lower_path, "lowercase-content"));
  EXPECT_TRUE(helpers::CheckFileContents(upper_path, "uppercase-content"));

  EXPECT_TRUE(
      helpers::CheckDirEntries(cs_dir, {L".", L"..", L"foo.txt", L"FOO.txt"}));

  // Disabling case sensitivity while case-differing names coexist must fail
  // with STATUS_CASE_DIFFERING_NAMES_IN_DIR.
  EXPECT_NT_STATUS(SetCaseSensitiveFlag(dh.value->get(), /*flags=*/0UL),
                   nt::kStatusCaseDifferingNamesInDir);

  ASSERT_WIN32_TRUE(::DeleteFileW(upper_path.c_str()));
  ASSERT_WIN32_TRUE(::DeleteFileW(lower_path.c_str()));

  // Once colliding names are removed, clearing the flag succeeds and persists.
  ASSERT_NT_STATUS(SetCaseSensitiveFlag(dh.value->get(), /*flags=*/0UL),
                   STATUS_SUCCESS);
  auto cleared = QueryCaseSensitiveFlag(dh.value->get());
  ASSERT_NT_STATUS(cleared.status, STATUS_SUCCESS);
  EXPECT_EQ(*cleared.value, 0UL);
}

// Spec: [MS-FSA] 2.1.5.1 / Win32 CreateFileW & FindFirstFileW -- NTFS compares
// raw UTF-16 code units without Unicode normalization, so canonically
// equivalent NFC and NFD filenames coexist as distinct files.
TEST_F(NamingPathsTest, UnicodeNfcAndNfdAreDistinctFiles) {
  const std::wstring sub = TestPath(L"nfc_nfd");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  // NFC: U+00E9 (precomposed e-acute). NFD: U+0065 + U+0301 (e + combining
  // acute). NTFS compares raw UTF-16 code units without normalization.
  const std::wstring nfc_name = L"caf\u00E9.txt";
  const std::wstring nfd_name = L"cafe\u0301.txt";
  ASSERT_NE(nfc_name, nfd_name);

  const std::wstring nfc_path = core::JoinPath(sub, nfc_name);
  const std::wstring nfd_path = core::JoinPath(sub, nfd_name);

  {
    auto h1 = core::CreateWin32File(nfc_path, GENERIC_WRITE, core::kShareAll,
                                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h1);
    ASSERT_WIN32_OK(core::WriteAllBytes(h1.value->get(), "nfc-bytes", 9));
  }
  {
    auto h2 = core::CreateWin32File(nfd_path, GENERIC_WRITE, core::kShareAll,
                                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h2);
    ASSERT_WIN32_OK(core::WriteAllBytes(h2.value->get(), "nfd-bytes", 9));
  }

  auto id_nfc = core::QueryPathFileIndex(nfc_path);
  auto id_nfd = core::QueryPathFileIndex(nfd_path);
  ASSERT_WIN32_OK(id_nfc);
  ASSERT_WIN32_OK(id_nfd);
  EXPECT_NE(*id_nfc.value, *id_nfd.value);

  auto rd_nfc = core::ReadFileContents(nfc_path);
  auto rd_nfd = core::ReadFileContents(nfd_path);
  ASSERT_WIN32_OK(rd_nfc);
  ASSERT_WIN32_OK(rd_nfd);
  EXPECT_EQ(*rd_nfc.value, "nfc-bytes");
  EXPECT_EQ(*rd_nfd.value, "nfd-bytes");

  auto listed = core::ListDirectoryWin32(sub, L"*");
  ASSERT_WIN32_OK(listed);
  const std::vector<std::wstring> expected = {L".", L"..", nfc_name, nfd_name};
  EXPECT_MULTISET_EQ(*listed.value, expected);
}

// Spec: Win32 CreateFileW path parsing -- before Windows 11 21H2 (build 22000)
// a path-qualified "<device>.<ext>" name (NUL.txt) is rewritten to the DOS
// device, so no such entry is created on the filesystem. Build 22000+ stopped
// this rewrite, hence the upper OS bound.
TEST_F(NamingPathsTest, Win32DosDeviceNameWithExtensionRedirectedBefore24H2) {
  REQUIRE_LOCAL_VOLUME();
  REQUIRE_WIN32_DOS_DEVICE_PARSING();
  REQUIRE_MAX_OS_BUILD(core::kBuildWin11_21H2 - 1);

  const std::wstring sub = TestPath(L"dos_ext_w32");
  const std::wstring sub_ext = TestExtendedPath(L"dos_ext_w32");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub_ext.c_str(), nullptr));

  const std::wstring win32_nul_txt = core::JoinPath(sub, L"NUL.txt");
  {
    auto nul_h = core::CreateWin32File(win32_nul_txt, GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(nul_h);
    ASSERT_WIN32_OK(core::WriteAllBytes(nul_h.value->get(), "null-sink", 9));
  }

  EXPECT_TRUE(helpers::CheckFileNotFound(core::JoinPath(sub_ext, L"NUL.txt")));
  EXPECT_TRUE(helpers::CheckDirEntries(sub_ext, {L".", L".."}));
}

// Spec: [MS-FSA] 2.1.5.1 / Win32 "\\?\" verbatim paths -- "<device>.<ext>"
// names (NUL.txt, CON.dat) are ordinary filenames to the filesystem: verbatim
// paths create, read, enumerate, and delete literal files on all OS builds.
TEST_F(NamingPathsTest, VerbatimDosDeviceNamesWithExtensionsAreLiteralFiles) {
  const std::wstring sub_ext = TestExtendedPath(L"dos_ext");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub_ext.c_str(), nullptr));

  const std::wstring verb_nul_txt = core::JoinPath(sub_ext, L"NUL.txt");
  const std::wstring verb_con_dat = core::JoinPath(sub_ext, L"CON.dat");

  ASSERT_WIN32_OK(core::WriteFileContents(verb_nul_txt, "literal-nul-txt"));
  ASSERT_WIN32_OK(core::WriteFileContents(verb_con_dat, "literal-con-dat"));

  EXPECT_TRUE(helpers::CheckFileContents(verb_nul_txt, "literal-nul-txt"));
  EXPECT_TRUE(helpers::CheckFileContents(verb_con_dat, "literal-con-dat"));

  EXPECT_TRUE(
      helpers::CheckDirEntries(sub_ext, {L".", L"..", L"NUL.txt", L"CON.dat"}));

  ASSERT_WIN32_TRUE(::DeleteFileW(verb_nul_txt.c_str()));
  ASSERT_WIN32_TRUE(::DeleteFileW(verb_con_dat.c_str()));
  EXPECT_TRUE(helpers::CheckDirEntries(sub_ext, {L".", L".."}));
}

// Spec: [MS-FSA] 2.1.5.1 / Win32 CreateFileW & FindFirstFileW -- leading
// spaces, leading dots ('.dotfile'), and verbatim '...' names are valid NTFS
// filenames that round-trip through create, enum, and delete.
TEST_F(NamingPathsTest, LeadingSpacesLeadingDotsAndTripleDotNames) {
  const std::wstring sub = TestPath(L"edge_names");
  const std::wstring sub_ext = TestExtendedPath(L"edge_names");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub_ext.c_str(), nullptr));

  const std::wstring lead_space = core::JoinPath(sub, L" leading.txt");
  const std::wstring dot_file = core::JoinPath(sub, L".dotfile");
  // Triple dot requires verbatim \\?\ so Win32 does not strip trailing dots.
  const std::wstring triple_dot = core::JoinPath(sub_ext, L"...");

  ASSERT_WIN32_OK(core::WriteFileContents(lead_space, "leading-space"));
  ASSERT_WIN32_OK(core::WriteFileContents(dot_file, "leading-dot"));
  ASSERT_WIN32_OK(core::WriteFileContents(triple_dot, "triple-dot"));

  EXPECT_TRUE(helpers::CheckFileContents(
      core::JoinPath(sub_ext, L" leading.txt"), "leading-space"));
  EXPECT_TRUE(helpers::CheckFileContents(core::JoinPath(sub_ext, L".dotfile"),
                                         "leading-dot"));
  EXPECT_TRUE(helpers::CheckFileContents(triple_dot, "triple-dot"));

  EXPECT_TRUE(helpers::CheckDirEntries(
      sub_ext, {L".", L"..", L" leading.txt", L".dotfile", L"..."}));

  ASSERT_WIN32_TRUE(
      ::DeleteFileW(core::JoinPath(sub_ext, L" leading.txt").c_str()));
  ASSERT_WIN32_TRUE(
      ::DeleteFileW(core::JoinPath(sub_ext, L".dotfile").c_str()));
  ASSERT_WIN32_TRUE(::DeleteFileW(triple_dot.c_str()));

  EXPECT_TRUE(helpers::CheckDirEntries(sub_ext, {L".", L".."}));
}

// Spec: [MS-FSA] 2.1.5.1 / Win32 CreateFileW, NtCreateFile &
// GetFileAttributesW -- appending a trailing backslash to an existing regular
// file path fails with ERROR_INVALID_NAME / STATUS_OBJECT_NAME_INVALID (pre-24H2
// Windows) or ERROR_DIRECTORY / STATUS_NOT_A_DIRECTORY (Win11 24H2+ / Server
// 2025+); either status is accepted across Windows builds.
TEST_F(NamingPathsTest, TrailingBackslashOnRegularFileFails) {
  const std::wstring file_path = TestPath(L"plain.dat");
  ASSERT_WIN32_OK(core::WriteFileContents(file_path, "regular-file"));

  const std::wstring with_slash = file_path + L"\\";
  auto open_res =
      core::CreateWin32File(with_slash, GENERIC_READ, core::kShareAll,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(open_res);
  EXPECT_TRUE(helpers::CheckTrailingSlashWin32Error(open_res.error))
      << "CreateFileW(OPEN_EXISTING)";
  if (open_res) {
    open_res.value->reset();
  }

  auto create_res =
      core::CreateWin32File(with_slash, GENERIC_WRITE, core::kShareAll,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(create_res);
  EXPECT_TRUE(helpers::CheckTrailingSlashWin32Error(create_res.error))
      << "CreateFileW(CREATE_NEW)";
  if (create_res) {
    create_res.value->reset();
  }

  auto nt_res = core::NtCreateFilePath(
      TestNtPath(L"plain.dat") + L"\\", FILE_READ_ATTRIBUTES | SYNCHRONIZE,
      FILE_ATTRIBUTE_NORMAL, core::kShareAll, nt::kFileOpen,
      nt::kFileNonDirectoryFile);
  EXPECT_FALSE(nt_res);
  EXPECT_TRUE(helpers::CheckTrailingSlashNtStatus(nt_res.status))
      << "NtCreateFile(FILE_OPEN)";
  if (nt_res) {
    nt_res.value->reset();
  }

  auto attrs = core::QueryWin32Attributes(with_slash);
  EXPECT_FALSE(attrs);
  EXPECT_TRUE(helpers::CheckTrailingSlashWin32Error(attrs.error))
      << "GetFileAttributesW";
}

// Spec: Win32 CreateFileW & GetFullPathNameW -- standard Win32 path parsing
// normalizes forward slashes and repeated separators to the same target
// FileIndex and file contents.
TEST_F(NamingPathsTest, MultipleConsecutiveSlashesAndForwardSlashesNormalized) {
  const std::wstring sub = TestPath(L"slash_sub");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));
  const std::wstring canonical = core::JoinPath(sub, L"file.txt");
  ASSERT_WIN32_OK(core::WriteFileContents(canonical, "slash-payload"));

  auto expected_id = core::QueryPathFileIndex(canonical);
  ASSERT_WIN32_OK(expected_id);

  const std::vector<std::wstring> variants = {
      sub + L"///file.txt",
      sub + L"/file.txt",
      sub + L"\\\\\\file.txt",
  };

  for (const auto &variant : variants) {
    auto id = core::QueryPathFileIndex(variant);
    ASSERT_WIN32_OK(id) << "Variant '" << core::WideToUtf8(variant)
                        << "' failed";
    EXPECT_EQ(*id.value, *expected_id.value);

    EXPECT_TRUE(helpers::CheckFileContents(variant, "slash-payload"));
  }
}

// Spec: Win32 SetFileShortNameW / [MS-FSA] 2.1.5.15.13 -- with
// SeRestorePrivilege and 8.3 enabled, SetFileShortNameW assigns a custom 8.3
// alias that resolves to the same FileIndex and cAlternateFileName.
TEST_F(NamingPathsTest, SetFileShortNameExplicit) {
  REQUIRE_SHORT_NAMES_ENABLED();
  REQUIRE_ADMIN();
  REQUIRE_PRIVILEGE(SE_RESTORE_NAME);

  core::ScopedPrivilege restore_priv(SE_RESTORE_NAME, true);
  ASSERT_TRUE(restore_priv.active())
      << "Failed to enable SeRestorePrivilege: "
      << core::FormatWin32Error(restore_priv.error());

  const std::wstring sub = TestPath(L"set_short");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  const std::wstring long_path =
      core::JoinPath(sub, L"custom_long_document_name.txt");
  ASSERT_WIN32_OK(core::WriteFileContents(long_path, "explicit-8dot3"));

  auto orig_id = core::QueryPathFileIndex(long_path);
  ASSERT_WIN32_OK(orig_id);

  {
    auto fh = OpenWithBackupSemantics(long_path, GENERIC_WRITE | DELETE);
    ASSERT_WIN32_OK(fh);
    ASSERT_WIN32_TRUE(::SetFileShortNameW(fh.value->get(), L"CUST~1.TXT"));
  }

  const std::wstring custom_short_path = core::JoinPath(sub, L"CUST~1.TXT");
  auto short_id = core::QueryPathFileIndex(custom_short_path);
  ASSERT_WIN32_OK(short_id);
  EXPECT_EQ(*short_id.value, *orig_id.value);

  EXPECT_TRUE(helpers::CheckFileContents(custom_short_path, "explicit-8dot3"));

  WIN32_FIND_DATAW data{};
  ::SetLastError(ERROR_SUCCESS);
  core::UniqueFindHandle find(::FindFirstFileW(long_path.c_str(), &data));
  const DWORD find_err = ::GetLastError();
  ASSERT_TRUE(find) << core::FormatWin32Error(find_err);
  EXPECT_TRUE(core::EqualsIgnoreCase(data.cAlternateFileName, L"CUST~1.TXT"))
      << "Expected cAlternateFileName CUST~1.TXT, got '"
      << core::WideToUtf8(data.cAlternateFileName) << "'";
}

// Spec: WDK FileCaseSensitiveInformation -- Windows 10 1903+ (build 18362+)
// rejects enabling per-directory case sensitivity on a non-empty directory with
// STATUS_DIRECTORY_NOT_EMPTY and leaves the flag clear. (Windows Server 2019,
// build 17763, was observed to accept the request, hence the lower OS bound.)
TEST_F(NamingPathsTest,
       PerDirectoryCaseSensitivityRejectedOnNonEmptyDirectory) {
  REQUIRE_MIN_OS_BUILD(core::kBuildWin10_1903);
  REQUIRE_LOCAL_VOLUME();
  REQUIRE_FS_CAPABILITY(FsCapability::kCaseSensitiveDir);
  REQUIRE_ADMIN();
  REQUIRE_HOST_CASE_SENSITIVE_DIR();

  const std::wstring non_empty = TestPath(L"cs_non_empty");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(non_empty.c_str(), nullptr));
  ASSERT_WIN32_OK(
      core::WriteFileContents(core::JoinPath(non_empty, L"seed.dat"), "seed"));

  EXPECT_NT_STATUS(
      SetCaseSensitiveFlagByPath(non_empty, nt::kFileCsFlagCaseSensitiveDir),
      STATUS_DIRECTORY_NOT_EMPTY);

  auto after = QueryCaseSensitiveFlagByPath(non_empty);
  ASSERT_NT_SUCCESS(after.status);
  EXPECT_EQ(*after.value & nt::kFileCsFlagCaseSensitiveDir, 0u);
}

// Spec: WDK FileCaseSensitiveInformation / [MS-FSA] 2.1.5.1 -- case-sensitive
// directories propagate the flag to new subdirectories (not moved-in ones) and
// enforce exact case even with OBJ_CASE_INSENSITIVE.
TEST_F(NamingPathsTest,
       PerDirectoryCaseSensitivityInheritanceAndMoveSemantics) {
  REQUIRE_MIN_OS_BUILD(core::kBuildWin10_1809);
  REQUIRE_LOCAL_VOLUME();
  REQUIRE_FS_CAPABILITY(FsCapability::kCaseSensitiveDir);
  REQUIRE_ADMIN();
  REQUIRE_HOST_CASE_SENSITIVE_DIR();

  // Enable on empty parent and close handle before MoveFileW (on Server 2022
  // and earlier, holding DELETE on the target parent conflicts with MoveFileW).
  const std::wstring parent_dir = TestPath(L"cs_inherit_parent");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(parent_dir.c_str(), nullptr));
  ASSERT_NT_SUCCESS(
      SetCaseSensitiveFlagByPath(parent_dir, nt::kFileCsFlagCaseSensitiveDir));
  auto parent_cs = QueryCaseSensitiveFlagByPath(parent_dir);
  ASSERT_NT_SUCCESS(parent_cs.status);
  EXPECT_NE(*parent_cs.value & nt::kFileCsFlagCaseSensitiveDir, 0u);

  // Newly created subdirectory inherits FILE_CS_FLAG_CASE_SENSITIVE_DIR.
  const std::wstring new_child = core::JoinPath(parent_dir, L"new_child");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(new_child.c_str(), nullptr));
  auto child_cs = QueryCaseSensitiveFlagByPath(new_child);
  ASSERT_NT_SUCCESS(child_cs.status);
  EXPECT_NE(*child_cs.value & nt::kFileCsFlagCaseSensitiveDir, 0u)
      << "Expected newly created child directory to inherit case sensitivity";

  const std::wstring child_mixed = core::JoinPath(new_child, L"Sample.txt");
  const std::wstring child_upper = core::JoinPath(new_child, L"SAMPLE.TXT");
  ASSERT_WIN32_OK(core::WriteFileContents(child_mixed, "child-mixed"));
  auto miss_upper = core::QueryPathFileIndex(child_upper);
  EXPECT_FALSE(miss_upper);
  EXPECT_WIN32_ERROR_CODE(miss_upper.error, ERROR_FILE_NOT_FOUND);

  ASSERT_WIN32_OK(core::WriteFileContents(child_upper, "child-upper"));
  auto id_mixed = core::QueryPathFileIndex(child_mixed);
  auto id_upper = core::QueryPathFileIndex(child_upper);
  ASSERT_WIN32_OK(id_mixed);
  ASSERT_WIN32_OK(id_upper);
  EXPECT_NE(*id_upper.value, *id_mixed.value);
  ASSERT_WIN32_TRUE(::DeleteFileW(child_mixed.c_str()));
  ASSERT_WIN32_TRUE(::DeleteFileW(child_upper.c_str()));

  // Pre-existing case-insensitive dir moved into case-sensitive parent retains
  // flags == 0.
  const std::wstring outside_dir = TestPath(L"cs_outside_ci");
  const std::wstring moved_child = core::JoinPath(parent_dir, L"moved_child");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(outside_dir.c_str(), nullptr));
  ASSERT_WIN32_TRUE(::MoveFileW(outside_dir.c_str(), moved_child.c_str()));
  auto moved_cs = QueryCaseSensitiveFlagByPath(moved_child);
  ASSERT_NT_SUCCESS(moved_cs.status);
  EXPECT_EQ(*moved_cs.value, 0UL)
      << "Expected moved case-insensitive directory to retain flags == 0";

  const std::wstring moved_mixed = core::JoinPath(moved_child, L"Item.txt");
  const std::wstring moved_upper = core::JoinPath(moved_child, L"ITEM.TXT");
  ASSERT_WIN32_OK(core::WriteFileContents(moved_mixed, "moved-insensitive"));
  auto moved_id_mixed = core::QueryPathFileIndex(moved_mixed);
  auto moved_id_upper = core::QueryPathFileIndex(moved_upper);
  ASSERT_WIN32_OK(moved_id_mixed);
  ASSERT_WIN32_OK(moved_id_upper);
  EXPECT_EQ(*moved_id_upper.value, *moved_id_mixed.value);
  EXPECT_TRUE(helpers::CheckFileContents(moved_upper, "moved-insensitive"));

  // Inside case-sensitive parent, both obj_attrs==0 and OBJ_CASE_INSENSITIVE
  // resolve exact case (Alpha.txt != alpha.txt).
  auto parent_h = OpenWithBackupSemantics(
      parent_dir, FILE_READ_ATTRIBUTES | FILE_TRAVERSE | SYNCHRONIZE);
  ASSERT_WIN32_OK(parent_h);

  ASSERT_TRUE(CheckCreateRelativeCaseFile(parent_h.value->get(), L"Alpha.txt",
                                          /*obj_attrs=*/0, "upper-alpha"));
  EXPECT_NT_STATUS(OpenRelativeCaseFile(parent_h.value->get(), L"alpha.txt",
                                        FILE_GENERIC_READ, nt::kFileOpen,
                                        /*obj_attrs=*/0)
                       .status,
                   STATUS_OBJECT_NAME_NOT_FOUND);
  EXPECT_NT_STATUS(OpenRelativeCaseFile(parent_h.value->get(), L"alpha.txt",
                                        FILE_GENERIC_READ, nt::kFileOpen,
                                        nt::kObjCaseInsensitive)
                       .status,
                   STATUS_OBJECT_NAME_NOT_FOUND);
  ASSERT_TRUE(CheckCreateRelativeCaseFile(parent_h.value->get(), L"alpha.txt",
                                          nt::kObjCaseInsensitive,
                                          "lower-alpha"));

  uint64_t idx_upper_cs = 0;
  uint64_t idx_lower_cs = 0;
  uint64_t idx_upper_ci = 0;
  uint64_t idx_lower_ci = 0;
  ASSERT_TRUE(CheckVerifyRelativeCaseFile(parent_h.value->get(), L"Alpha.txt",
                                          /*obj_attrs=*/0, "upper-alpha",
                                          &idx_upper_cs));
  ASSERT_TRUE(CheckVerifyRelativeCaseFile(parent_h.value->get(), L"alpha.txt",
                                          /*obj_attrs=*/0, "lower-alpha",
                                          &idx_lower_cs));
  ASSERT_TRUE(CheckVerifyRelativeCaseFile(parent_h.value->get(), L"Alpha.txt",
                                          nt::kObjCaseInsensitive,
                                          "upper-alpha", &idx_upper_ci));
  ASSERT_TRUE(CheckVerifyRelativeCaseFile(parent_h.value->get(), L"alpha.txt",
                                          nt::kObjCaseInsensitive,
                                          "lower-alpha", &idx_lower_ci));

  EXPECT_NE(idx_upper_cs, idx_lower_cs);
  EXPECT_EQ(idx_upper_cs, idx_upper_ci);
  EXPECT_EQ(idx_lower_cs, idx_lower_ci);

  ASSERT_WIN32_TRUE(
      ::DeleteFileW(core::JoinPath(parent_dir, L"Alpha.txt").c_str()));
  ASSERT_WIN32_TRUE(
      ::DeleteFileW(core::JoinPath(parent_dir, L"alpha.txt").c_str()));
}

// Spec: Win32 CreateFileW & GetFullPathNameW -- Win32 path normalization clamps
// excess '..' components at the drive volume root so paths escaping above X:\
// resolve to the same on-volume target.
TEST_F(NamingPathsTest, DotDotTraversalClampsAtVolumeRoot) {
  REQUIRE_VOLUME_ROOT();
  REQUIRE_LOCAL_VOLUME();
  if (Root().kind != core::RootKind::kDriveLetter) {
    RecordSkipCategory("volume_root");
    GTEST_SKIP() << "Requires a drive-letter volume root (X:\\) for Win32 "
                    "'..' root clamping.";
  }

  const std::wstring target = TestPath(L"clamp_target.dat");
  const std::string payload = "dotdot-root-clamp-payload";
  ASSERT_WIN32_OK(core::WriteFileContents(target, payload));

  auto expected_id = core::QueryPathFileIndex(target);
  ASSERT_WIN32_OK(expected_id);

  std::wstring_view temp_path = TempDir().Path();
  if (temp_path.rfind(L"\\\\?\\", 0) == 0) {
    temp_path.remove_prefix(4);
  }
  ASSERT_GE(temp_path.size(), size_t{4})
      << "Unexpected drive-root TempDir path: " << core::WideToUtf8(temp_path);
  ASSERT_EQ(temp_path[1], L':');
  ASSERT_EQ(temp_path[2], L'\\');
  const std::wstring drive_root(temp_path.substr(0, 3));
  const std::wstring rel_temp(temp_path.substr(3));
  const std::wstring rel_target = core::JoinPath(rel_temp, L"clamp_target.dat");

  const std::vector<std::wstring> clamped_paths = {
      drive_root + L"..\\..\\..\\..\\" + rel_target,
      drive_root + rel_temp + L"\\..\\..\\..\\..\\" + rel_target,
  };

  for (const auto &clamped : clamped_paths) {
    auto id = core::QueryPathFileIndex(clamped);
    ASSERT_WIN32_OK(id) << "Clamped path '" << core::WideToUtf8(clamped)
                        << "' failed";
    EXPECT_EQ(*id.value, *expected_id.value);

    EXPECT_TRUE(helpers::CheckFileContents(clamped, payload));
  }
}

// Spec: [MS-FSA] 2.1.5.1.1 / Win32 CreateFileW, DeleteFileW & MoveFileExW --
// NTFS 15-second metadata tunneling preserves CreationTime when a file is
// deleted or renamed away and immediately recreated with the same name.
TEST_F(NamingPathsTest, MetadataTunnelingPreservesCreationTime) {
  REQUIRE_LOCAL_VOLUME();

  std::string disable_reason;
  if (IsMetadataTunnelingDisabledByRegistry(&disable_reason)) {
    RecordSkipCategory("os_policy");
    GTEST_SKIP() << disable_reason;
  }

  const std::wstring sub = TestPath(L"tunnel_sub");
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  // Case 1: Delete followed by immediate recreation preserves CreationTime.
  {
    const std::wstring path = core::JoinPath(sub, L"tunnel_del.txt");
    ASSERT_WIN32_OK(core::WriteFileContents(path, "before_delete"));

    const int64_t backdate = core::MakeUtcFileTimeInt64(2015, 1, 1);
    ASSERT_TRUE(fstest::BackdateAllTimes(path, backdate));

    auto before_info = helpers::QueryPathBasicInfo(path);
    ASSERT_TRUE(before_info) << before_info.Describe();
    ASSERT_EQ(before_info.value->creation_time.QuadPart, backdate);

    ASSERT_WIN32_TRUE(::DeleteFileW(path.c_str()));

    auto h_new = core::CreateWin32File(path, GENERIC_WRITE, core::kShareAll,
                                       CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h_new);
    ASSERT_WIN32_OK(
        core::WriteAllBytes(h_new.value->get(), "after_delete", 12));
    h_new.value->reset();

    auto after_info = helpers::QueryPathBasicInfo(path);
    ASSERT_TRUE(after_info) << after_info.Describe();
    EXPECT_EQ(after_info.value->creation_time.QuadPart, backdate);
    EXPECT_GT(after_info.value->last_write_time.QuadPart, backdate);
    EXPECT_TRUE(helpers::CheckFileContents(path, "after_delete"));
  }

  // Case 2: Rename-out followed by immediate recreation ("safe-save" pattern)
  // preserves CreationTime on the recreated file with a distinct FileIndex.
  {
    const std::wstring path = core::JoinPath(sub, L"tunnel_ren.txt");
    const std::wstring moved = core::JoinPath(sub, L"tunnel_ren_old.txt");
    ASSERT_WIN32_OK(core::WriteFileContents(path, "before_rename"));

    const int64_t backdate = core::MakeUtcFileTimeInt64(2016, 6, 15);
    ASSERT_TRUE(fstest::BackdateAllTimes(path, backdate));

    auto orig_idx = core::QueryPathFileIndex(path);
    ASSERT_WIN32_OK(orig_idx);

    ASSERT_WIN32_TRUE(::MoveFileExW(path.c_str(), moved.c_str(), 0));

    auto h_new = core::CreateWin32File(path, GENERIC_WRITE, core::kShareAll,
                                       CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
    ASSERT_WIN32_OK(h_new);
    ASSERT_WIN32_OK(core::WriteAllBytes(h_new.value->get(), "safe_saved", 10));
    h_new.value->reset();

    auto new_idx = core::QueryPathFileIndex(path);
    ASSERT_WIN32_OK(new_idx);
    EXPECT_NE(*new_idx.value, *orig_idx.value);

    auto after_info = helpers::QueryPathBasicInfo(path);
    ASSERT_TRUE(after_info) << after_info.Describe();
    EXPECT_EQ(after_info.value->creation_time.QuadPart, backdate);
    EXPECT_GT(after_info.value->last_write_time.QuadPart, backdate);
    EXPECT_TRUE(helpers::CheckFileContents(path, "safe_saved"));
  }
}

// Spec: [MS-FSA] 2.1.5.1 / Win32 CreateFileW & NtOpenFile -- verbatim
// (`\\?\` and `\??\`) paths bypass Win32 path canonicalization so literal ".."
// segments reach the filesystem driver and are rejected with ERROR_INVALID_NAME
// / STATUS_OBJECT_NAME_INVALID.
TEST_F(NamingPathsTest, VerbatimDotDotTraversalRejected) {
  const std::wstring canary = TestPath(L"canary.txt");
  const std::wstring sub = TestPath(L"sub");
  ASSERT_WIN32_OK(core::WriteFileContents(canary, "canary"));
  ASSERT_WIN32_TRUE(::CreateDirectoryW(sub.c_str(), nullptr));

  // Strictly read-only OPEN_EXISTING / FILE_OPEN probes so no out-of-root
  // writes can occur even on a broken driver that resolves "..".
  const std::wstring ext_dotdot = TestExtendedPath(L"sub\\..\\canary.txt");
  auto win_res =
      core::CreateWin32File(ext_dotdot, FILE_READ_ATTRIBUTES, core::kShareAll,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
  EXPECT_FALSE(win_res);
  EXPECT_WIN32_ERROR_CODE(win_res.error, ERROR_INVALID_NAME);
  if (win_res) {
    win_res.value->reset();
  }

  const std::wstring nt_dotdot = TestVerbatimNtPath(L"sub\\..\\canary.txt");
  auto nt_res = core::NtOpenFilePath(
      nt_dotdot, FILE_READ_ATTRIBUTES | SYNCHRONIZE, core::kShareAll,
      nt::kFileNonDirectoryFile | nt::kFileSynchronousIoNonalert);
  EXPECT_FALSE(nt_res);
  EXPECT_NT_STATUS(nt_res.status, STATUS_OBJECT_NAME_INVALID);
  if (nt_res) {
    nt_res.value->reset();
  }
}

void ForceLink_NamingPaths() {}

} // namespace fstest
