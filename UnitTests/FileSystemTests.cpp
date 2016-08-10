#include "stdafx.h"
#include "CppUnitTest.h"

#include <Windows.h>
#include <Shlwapi.h>
#include <sddl.h>

#include <string>
#include <cassert>
#include <vector>
#include <memory>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

#define FILESYSTEM_ROOT L"M:\\"
#define FILESYSTEM_TESTDIR L"M:\\MirrorUnitTests"

#define FILESYSTEM_ROOT_A "M:\\"
#define FILESYSTEM_TESTDIR_A "M:\\MirrorUnitTests"

const wchar_t* GetErrorCodeStr(const DWORD errCode);

#define USE_MIRROR 1

int fsx_main(int argc, char **argv);

namespace UnitTests
{		
	TEST_CLASS(FileSystemTests)
	{
	private:

		inline bool IsValidHandle(HANDLE handle) { return handle && handle != INVALID_HANDLE_VALUE; }

		std::unique_ptr<BYTE[]> GetSecurityForFile(
			const wchar_t *filePath,
			const SECURITY_INFORMATION requestedInformation,
			DWORD &securitySize)
		{
			std::wstring errMsg;

			if(!GetFileSecurityW(filePath, requestedInformation, nullptr, 0, &securitySize) && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to retrieve security information size for file \'%s\'.", filePath));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			std::unique_ptr<BYTE[]> securityDesc = std::make_unique<BYTE[]>(securitySize);

			if(!GetFileSecurityW(filePath, DACL_SECURITY_INFORMATION, (PSECURITY_DESCRIPTOR)securityDesc.get(), securitySize, &securitySize))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to retrieve security information for file \'%s\'.", filePath));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			return securityDesc;
		}

		std::wstring SecurityDescriptorToString(const PSECURITY_DESCRIPTOR security, const SECURITY_INFORMATION requestedInformation)
		{
			LPWSTR buf;

			if(!ConvertSecurityDescriptorToStringSecurityDescriptorW(
				security,
				SDDL_REVISION_1,
				requestedInformation,
				&buf,
				nullptr))
			{
				std::wstring errMsg = CreateSystemErrorMessage(FormatString(
					L"Failed to convert a security descriptor with requested information 0x%x into its string representation.",
					requestedInformation));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			std::wstring retStr = buf;

			LocalFree(buf);

			return retStr;
		}

		std::unique_ptr<BYTE[]> SecurityDescriptorFromString(const wchar_t *str,  ULONG &size)
		{
			PSECURITY_DESCRIPTOR temp;

			if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(
				str,
				SDDL_REVISION_1,
				&temp,
				&size))
			{
				std::wstring errMsg = CreateSystemErrorMessage(FormatString(
					L"Failed to convert string \'%s\' into a security descriptor.",
					str));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			std::unique_ptr<BYTE[]> securityDesc = std::make_unique<BYTE[]>(size);

			memcpy_s(securityDesc.get(), size, temp, size);

			LocalFree(temp);

			return securityDesc;
		}

		std::wstring GetTempPathStr()
		{
			WCHAR path[MAX_PATH];

			GetTempPathW(MAX_PATH, path);

			return path;
		}

		std::string GetTempPathStrA()
		{
			char path[MAX_PATH];

			GetTempPathA(MAX_PATH, path);

			return path;
		}

		std::wstring CombinePath(const std::wstring &left, const std::wstring &right)
		{
			if(left.length() == 0)
			{
				return right;
			}

			if(right.length() == 0)
			{
				return left;
			}

			const bool leftHasDelimeter = left[left.length() - 1] == L'\\' || left[left.length() - 1] == L'/';
			const bool rightHasDelimeter = right[right.length() - 1] == L'\\' || right[right.length() - 1] == L'/';

			if(leftHasDelimeter && rightHasDelimeter)
			{
				return left.substr(0, left.length() - 1) + right;
			}
			else if(!leftHasDelimeter && !rightHasDelimeter)
			{
				return left + L"\\" + right;
			}

			return left + right;
		}

		std::string CombinePath(const std::string &left, const std::string &right)
		{
			if(left.length() == 0)
			{
				return right;
			}

			if(right.length() == 0)
			{
				return left;
			}

			const bool leftHasDelimeter = left[left.length() - 1] == '\\' || left[left.length() - 1] == '/';
			const bool rightHasDelimeter = right[right.length() - 1] == '\\' || right[right.length() - 1] == '/';

			if(leftHasDelimeter && rightHasDelimeter)
			{
				return left.substr(0, left.length() - 1) + right;
			}
			else if(!leftHasDelimeter && !rightHasDelimeter)
			{
				return left + "\\" + right;
			}

			return left + right;
		}

		std::wstring CreateTestDirectory()
		{
#if USE_MIRROR
			std::wstring testDir = FILESYSTEM_TESTDIR;
#else
			std::wstring testDir = CombinePath(GetTempPathStr(), L"MirrorUnitTests");
#endif

			if(!PathFileExistsW(testDir.c_str()))
			{
				if(!CreateDirectoryW(testDir.c_str(), nullptr))
				{
					std::wstring errMsg = CreateSystemErrorMessage(FormatString(L"Failed to CreateDirectoryW for directory \'%s\'.", testDir.c_str()));

					Assert::Fail(errMsg.c_str(), LINE_INFO());
				}
			}

			return testDir;
		}

		std::string CreateTestDirectoryA()
		{
#if USE_MIRROR
			std::string testDir = FILESYSTEM_TESTDIR_A;
#else
			std::string testDir = CombinePath(GetTempPathStrA(), "MirrorUnitTests");
#endif

			if(!PathFileExistsA(testDir.c_str()))
			{
				if(!CreateDirectoryA(testDir.c_str(), nullptr))
				{
					std::wstring errMsg = CreateSystemErrorMessage(FormatString(L"Failed to CreateDirectoryW for directory \'%s\'.", testDir.c_str()));

					Assert::Fail(errMsg.c_str(), LINE_INFO());
				}
			}

			return testDir;
		}

		void DeleteTestDirectory()
		{
#if USE_MIRROR
			std::wstring testDir = FILESYSTEM_TESTDIR;
#else
			std::wstring testDir = CombinePath(GetTempPathStr(), L"MirrorUnitTests");
#endif

			if(PathFileExistsW(testDir.c_str()))
			{
				if(!RemoveDirectoryW(testDir.c_str()))
				{
					std::wstring errMsg = CreateSystemErrorMessage(FormatString(L"Failed to RemoveDirectoryW for directory \'%s\'.", testDir.c_str()));

					Assert::Fail(errMsg.c_str(), LINE_INFO());
				}
			}
		}

		std::wstring FormatString(const wchar_t *str, ...)
		{
			size_t size = 256;
			wchar_t *buf = static_cast<wchar_t*>(malloc(size * sizeof(wchar_t)));
			int written = 0;

			va_list args;
			va_start(args, str);

			while((written = _vsnwprintf_s(buf, size - 1, size - 1, str, args)) == -1)
			{
				size *= 2;
				buf = static_cast<wchar_t*>(realloc(buf, size * sizeof(wchar_t)));

				if(!buf)
				{
					written = 0;
					break;
				}
			}

			va_end(args);

			assert(static_cast<size_t>(written) <= size - 2);

			buf[written] = 0;

			std::wstring temp = buf;

			free(buf);

			return temp;
		}

		std::string FormatString(const char *str, ...)
		{
			size_t size = 256;
			char *buf = static_cast<char*>(malloc(size));
			int written = 0;

			va_list args;
			va_start(args, str);

			while((written = _vsnprintf_s(buf, size - 1, size - 1, str, args)) == -1)
			{
				size *= 2;
				buf = static_cast<char*>(realloc(buf, size));

				if(!buf)
				{
					written = 0;
					break;
				}
			}

			va_end(args);

			assert(static_cast<size_t>(written) <= size - 2);

			buf[written] = 0;

			std::string temp = buf;

			free(buf);

			return temp;
		}

		std::wstring GetLastErrorStr(const DWORD lastErr)
		{
			LPWSTR buffer = nullptr;

			// we force english because the error messages are for developers and english is the common language
			// developers of all countries use to communicate
			DWORD errCode = FormatMessageW(
				FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
				nullptr,
				lastErr,
				MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
				(LPWSTR)&buffer,
				0,
				nullptr);

			if(errCode == 0)
			{
				if(GetLastError() == ERROR_RESOURCE_LANG_NOT_FOUND)
				{
					errCode = FormatMessageW(
						FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
						nullptr,
						lastErr,
						0,
						(LPWSTR)&buffer,
						0,
						nullptr);
				}

				if(errCode == 0)
				{
					return std::wstring();
				}
			}

			std::wstring retStr = buffer;

			LocalFree(buffer);

			return retStr;
		}

		std::wstring CreateSystemErrorMessage(const std::wstring &msg)
		{
			const DWORD errCode = GetLastError();
			const std::wstring errCodeMsg = GetLastErrorStr(errCode);

			return FormatString(L"%s\nError code is \'%s\' (%d).\n%s",
				msg.c_str(),
				GetErrorCodeStr(errCode),
				errCode,
				errCodeMsg.c_str());
		}

	public:
		
		TEST_METHOD(TestFileBasicInfo)
		{
			std::wstring testDir = CreateTestDirectory();
			std::wstring testFile = CombinePath(testDir, L"TestFileBasicInfo.txt");

			DWORD fileDesiredAccess =
				FILE_READ_ATTRIBUTES
				| READ_CONTROL
				| FILE_WRITE_DATA
				| FILE_WRITE_ATTRIBUTES
				| FILE_WRITE_EA
				| FILE_APPEND_DATA
				| SYNCHRONIZE
				| STANDARD_RIGHTS_READ
				| STANDARD_RIGHTS_WRITE
				| STANDARD_RIGHTS_EXECUTE;

			HANDLE handle = CreateFileW(testFile.c_str(), fileDesiredAccess, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

			if(!IsValidHandle(handle))
			{
				std::wstring errMsg = CreateSystemErrorMessage(FormatString(L"Failed to create file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			BY_HANDLE_FILE_INFORMATION fileInfo;
			ZeroMemory(&fileInfo, sizeof(fileInfo));

			if(!GetFileInformationByHandle(handle, &fileInfo))
			{
				std::wstring errMsg = CreateSystemErrorMessage(FormatString(L"Failed to GetFileInformationByHandle for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			FILE_BASIC_INFO basicInfo;
			basicInfo.ChangeTime = *reinterpret_cast<LARGE_INTEGER*>(&fileInfo.ftLastWriteTime);
			basicInfo.CreationTime = *reinterpret_cast<LARGE_INTEGER*>(&fileInfo.ftCreationTime);
			basicInfo.FileAttributes = fileInfo.dwFileAttributes;
			basicInfo.LastAccessTime = *reinterpret_cast<LARGE_INTEGER*>(&fileInfo.ftLastAccessTime);
			basicInfo.LastWriteTime = *reinterpret_cast<LARGE_INTEGER*>(&fileInfo.ftLastWriteTime);

			basicInfo.CreationTime.QuadPart -= 500;

			if(!SetFileInformationByHandle(handle, FileBasicInfo, &basicInfo, sizeof(basicInfo)))
			{
				std::wstring errMsg = CreateSystemErrorMessage(FormatString(L"Failed to SetFileInformationByHandle for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			BY_HANDLE_FILE_INFORMATION fileInfo2;
			ZeroMemory(&fileInfo2, sizeof(fileInfo2));

			if(!GetFileInformationByHandle(handle, &fileInfo2))
			{
				std::wstring errMsg = CreateSystemErrorMessage(FormatString(L"Failed to GetFileInformationByHandle for modified file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			Assert::IsTrue(((LARGE_INTEGER*)&fileInfo2.ftCreationTime)->QuadPart == basicInfo.CreationTime.QuadPart,
				L"Failed to properly set modified file creation time.",
				LINE_INFO());

			if(!CloseHandle(handle))
			{
				std::wstring errMsg = CreateSystemErrorMessage(FormatString(L"Failed to close handle for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!DeleteFileW(testFile.c_str()))
			{
				std::wstring errMsg = CreateSystemErrorMessage(FormatString(L"Failed DeleteFileW for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			DeleteTestDirectory();
		}

		// This is test 01 in winfstest
		TEST_METHOD(TestCreateModifyAttribsDelete)
		{
			std::wstring testDir = CreateTestDirectory();
			std::wstring testFile = CombinePath(testDir, L"TestCreateModifyAttribsDelete.txt");
			std::wstring errMsg;

			DWORD fileDesiredAccess =
				FILE_READ_ATTRIBUTES
				| READ_CONTROL
				| FILE_WRITE_DATA
				| FILE_WRITE_ATTRIBUTES
				| FILE_WRITE_EA
				| FILE_APPEND_DATA
				| SYNCHRONIZE
				| STANDARD_RIGHTS_READ
				| STANDARD_RIGHTS_WRITE
				| STANDARD_RIGHTS_EXECUTE;

			HANDLE handle = CreateFileW(testFile.c_str(), fileDesiredAccess, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

			if(!IsValidHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to create file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			BY_HANDLE_FILE_INFORMATION fileInfo;
			ZeroMemory(&fileInfo, sizeof(fileInfo));

			if(!GetFileInformationByHandle(handle, &fileInfo))
			{
				CloseHandle(handle);

				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to GetFileInformationByHandle for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if((fileInfo.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_READONLY)) != 0)
			{
				CloseHandle(handle);

				errMsg = CreateSystemErrorMessage(FormatString(L"FILE_ATTRIBUTE_NORMAL should be set for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!CloseHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to close handle for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			handle = CreateFileW(testFile.c_str(), fileDesiredAccess, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_READONLY, nullptr);

			if(!IsValidHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to create file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			ZeroMemory(&fileInfo, sizeof(fileInfo));

			if(!GetFileInformationByHandle(handle, &fileInfo))
			{
				CloseHandle(handle);

				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to GetFileInformationByHandle for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if((fileInfo.dwFileAttributes & FILE_ATTRIBUTE_READONLY) == 0)
			{
				CloseHandle(handle);

				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to set FILE_ATTRIBUTE_READONLY for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}
			
			if(!CloseHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to close handle for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!SetFileAttributesW(testFile.c_str(), FILE_ATTRIBUTE_NORMAL))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed SetFileAttributesW() to set FILE_ATTRIBUTE_NORMAL for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			handle = CreateFileW(testFile.c_str(), fileDesiredAccess, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_SYSTEM, nullptr);

			if(!IsValidHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to create file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			ZeroMemory(&fileInfo, sizeof(fileInfo));

			if(!GetFileInformationByHandle(handle, &fileInfo))
			{
				CloseHandle(handle);

				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to GetFileInformationByHandle for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if((fileInfo.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) == 0)
			{
				CloseHandle(handle);

				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to set FILE_ATTRIBUTE_SYSTEM for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!CloseHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to close handle for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!SetFileAttributesW(testFile.c_str(), FILE_ATTRIBUTE_NORMAL))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed SetFileAttributesW() to set FILE_ATTRIBUTE_NORMAL for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if((GetFileAttributesW(testFile.c_str()) & FILE_ATTRIBUTE_NORMAL) == 0)
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to set FILE_ATTRIBUTE_NORMAL for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			handle = CreateFileW(testFile.c_str(), fileDesiredAccess, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);

			if(!IsValidHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to create file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			ZeroMemory(&fileInfo, sizeof(fileInfo));

			if(!GetFileInformationByHandle(handle, &fileInfo))
			{
				CloseHandle(handle);

				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to GetFileInformationByHandle for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if((fileInfo.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) == 0)
			{
				CloseHandle(handle);

				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to set FILE_ATTRIBUTE_HIDDEN for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!CloseHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to close handle for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!DeleteFileW(testFile.c_str()))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to delete file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			handle = CreateFileW(testFile.c_str(), fileDesiredAccess, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_READONLY, nullptr);

			if(!IsValidHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to create file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!CloseHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to close handle for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(DeleteFileW(testFile.c_str()) || GetLastError() != ERROR_ACCESS_DENIED)
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Successfully deleted file \'%s\' when it was expected to fail.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!SetFileAttributesW(testFile.c_str(), FILE_ATTRIBUTE_NORMAL))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed SetFileAttributesW() to set FILE_ATTRIBUTE_NORMAL for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!DeleteFileW(testFile.c_str()))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to delete file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			DeleteTestDirectory();
		}

		// This is test 10 in winfstest
		TEST_METHOD(TestFileSecurity)
		{
			const wchar_t *DESCRIPTOR1 = L"D:P(A;;GA;;;WD)";
			const wchar_t *DESCRIPTOR1_COMP = L"D:P(A;;FA;;;WD)";

			const wchar_t *DESCRIPTOR2 = L"D:P(A;;GA;;;WD)(A;;GR;;;SY)";
			const wchar_t *DESCRIPTOR2_COMP = L"D:P(A;;FA;;;WD)(A;;FR;;;SY)";

			const wchar_t *DESCRIPTOR3 = L"D:P(D;;GR;;;WD)";

			std::wstring testDir = CreateTestDirectory();
			std::wstring testFile = CombinePath(testDir, L"TestFileSecurity.txt");
			std::wstring errMsg;
			DWORD securitySize;
			std::unique_ptr<BYTE[]> securityDesc = SecurityDescriptorFromString(DESCRIPTOR1, securitySize);
			SECURITY_ATTRIBUTES securityAttrib;

			securityAttrib.nLength = sizeof(securityAttrib);
			securityAttrib.lpSecurityDescriptor = reinterpret_cast<PSECURITY_DESCRIPTOR>(securityDesc.get());
			securityAttrib.bInheritHandle = FALSE;

			HANDLE handle = CreateFileW(
				testFile.c_str(),
				GENERIC_WRITE,
				0,
				&securityAttrib,
				CREATE_NEW,
				FILE_ATTRIBUTE_NORMAL,
				nullptr);

			if(!IsValidHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to create file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!CloseHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to close handle for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}
			
			securityDesc = GetSecurityForFile(testFile.c_str(), DACL_SECURITY_INFORMATION, securitySize);

			Assert::IsTrue(securityDesc.operator bool(), L"Invalid security descriptor.", LINE_INFO());

			std::wstring securityDescStr = SecurityDescriptorToString(
				reinterpret_cast<PSECURITY_DESCRIPTOR>(securityDesc.get()),
				DACL_SECURITY_INFORMATION);

			if(securityDescStr != DESCRIPTOR1_COMP)
			{
				errMsg = FormatString(L"Security descriptor \'%s\' for file \'%s\' did not equal \'%s\'.",
					securityDescStr.c_str(),
					testFile.c_str(),
					DESCRIPTOR1_COMP);

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			securityDesc = SecurityDescriptorFromString(DESCRIPTOR2, securitySize);

			if(!SetFileSecurityW(
				testFile.c_str(),
				DACL_SECURITY_INFORMATION,
				reinterpret_cast<PSECURITY_DESCRIPTOR>(securityDesc.get())))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to set security descriptor \'%s\' for file \'%s\'.",
					DESCRIPTOR2,
					testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			securityDesc = GetSecurityForFile(testFile.c_str(), DACL_SECURITY_INFORMATION, securitySize);

			securityDescStr = SecurityDescriptorToString(
				reinterpret_cast<PSECURITY_DESCRIPTOR>(securityDesc.get()),
				DACL_SECURITY_INFORMATION);

			if(securityDescStr != DESCRIPTOR2_COMP)
			{
				errMsg = FormatString(L"Security descriptor \'%s\' for file \'%s\' did not equal \'%s\'.",
					securityDescStr.c_str(),
					testFile.c_str(),
					DESCRIPTOR2_COMP);

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!DeleteFileW(testFile.c_str()))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to delete file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			securityDesc = SecurityDescriptorFromString(DESCRIPTOR1, securitySize);
			securityAttrib.lpSecurityDescriptor = reinterpret_cast<PSECURITY_DESCRIPTOR>(securityDesc.get());

			handle = CreateFileW(
				testFile.c_str(),
				GENERIC_WRITE,
				0,
				&securityAttrib,
				CREATE_NEW,
				FILE_ATTRIBUTE_NORMAL,
				nullptr);

			if(!IsValidHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to create file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!CloseHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to close handle for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			handle = CreateFileW(
				testFile.c_str(),
				GENERIC_READ,
				0,
				nullptr,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL,
				nullptr);

			if(!IsValidHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to create file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!CloseHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to close handle for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			securityDesc = SecurityDescriptorFromString(DESCRIPTOR3, securitySize);

			if(!SetFileSecurityW(
				testFile.c_str(),
				DACL_SECURITY_INFORMATION,
				reinterpret_cast<PSECURITY_DESCRIPTOR>(securityDesc.get())))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to set security descriptor \'%s\' for file \'%s\'.",
					DESCRIPTOR3,
					testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			handle = CreateFileW(
				testFile.c_str(),
				GENERIC_READ,
				0,
				nullptr,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL,
				nullptr);

			if(IsValidHandle(handle) || GetLastError() != ERROR_ACCESS_DENIED)
			{
				if(IsValidHandle(handle))
				{
					CloseHandle(handle);
				}

				errMsg = CreateSystemErrorMessage(FormatString(L"File \'%s\' was expected to fail opening with ERROR_ACCESS_DENIED.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!DeleteFileW(testFile.c_str()))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to delete file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			securityDesc = SecurityDescriptorFromString(DESCRIPTOR1, securitySize);
			securityAttrib.lpSecurityDescriptor = reinterpret_cast<PSECURITY_DESCRIPTOR>(securityDesc.get());

			if(!CreateDirectoryW(testFile.c_str(), &securityAttrib))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to create directory \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			securityDesc = GetSecurityForFile(testFile.c_str(), DACL_SECURITY_INFORMATION, securitySize);

			Assert::IsTrue(securityDesc.operator bool(), L"Invalid security descriptor.", LINE_INFO());

			securityDescStr = SecurityDescriptorToString(
				reinterpret_cast<PSECURITY_DESCRIPTOR>(securityDesc.get()),
				DACL_SECURITY_INFORMATION);

			if(securityDescStr != DESCRIPTOR1_COMP)
			{
				errMsg = FormatString(L"Security descriptor \'%s\' for directory \'%s\' did not equal \'%s\'.",
					securityDescStr.c_str(),
					testFile.c_str(),
					DESCRIPTOR1_COMP);

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			securityDesc = SecurityDescriptorFromString(DESCRIPTOR2, securitySize);

			if(!SetFileSecurityW(
				testFile.c_str(),
				DACL_SECURITY_INFORMATION,
				reinterpret_cast<PSECURITY_DESCRIPTOR>(securityDesc.get())))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to set security descriptor \'%s\' for directory \'%s\'.",
					DESCRIPTOR2,
					testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			securityDesc = GetSecurityForFile(testFile.c_str(), DACL_SECURITY_INFORMATION, securitySize);

			securityDescStr = SecurityDescriptorToString(
				reinterpret_cast<PSECURITY_DESCRIPTOR>(securityDesc.get()),
				DACL_SECURITY_INFORMATION);

			if(securityDescStr != DESCRIPTOR2_COMP)
			{
				errMsg = FormatString(L"Security descriptor \'%s\' for directory \'%s\' did not equal \'%s\'.",
					securityDescStr.c_str(),
					testFile.c_str(),
					DESCRIPTOR2_COMP);

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!RemoveDirectoryW(testFile.c_str()))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed remove directory \'%s\'.",
					testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			DeleteTestDirectory();
		}

		// This is stream test 02 in winfstest
		TEST_METHOD(TestFileStreams02)
		{
			std::wstring testDir = CreateTestDirectory();
			std::wstring testFile = CombinePath(testDir, L"TestFileStreams02.txt");
			std::wstring errMsg;

			DWORD fileDesiredAccess =
				FILE_READ_ATTRIBUTES
				| READ_CONTROL
				| FILE_WRITE_DATA
				| FILE_WRITE_ATTRIBUTES
				| FILE_WRITE_EA
				| FILE_APPEND_DATA
				| SYNCHRONIZE
				| STANDARD_RIGHTS_READ
				| STANDARD_RIGHTS_WRITE
				| STANDARD_RIGHTS_EXECUTE;

			HANDLE handle = CreateFileW(testFile.c_str(), fileDesiredAccess, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);

			if(!IsValidHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to create file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!CloseHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to close handle for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			WIN32_FIND_STREAM_DATA findStreamData;
			std::vector<WIN32_FIND_STREAM_DATA> streams;

			handle = FindFirstStreamW(testFile.c_str(), FindStreamInfoStandard, &findStreamData, 0);

			if(!IsValidHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to find streams for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			do
			{
				streams.push_back(findStreamData);

			} while(FindNextStreamW(handle, &findStreamData));

			if(!FindClose(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to close streams handle for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(streams.size() != 1)
			{
				errMsg = FormatString(
					L"Invalid number of streams (%u) for file \'%s\' - expected 1 stream.",
					static_cast<DWORD>(streams.size()),
					testFile.c_str());

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(wcscmp(streams[0].cStreamName, L"::$DATA") != 0)
			{
				errMsg = FormatString(
					L"Expected stream \'::$DATA\' and instead got stream \'%s\' for file \'%s\'.",
					streams[0].cStreamName,
					testFile.c_str());

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!DeleteFileW(testFile.c_str()))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to delete file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			handle = FindFirstStreamW(testFile.c_str(), FindStreamInfoStandard, &findStreamData, 0);

			if((handle && handle != INVALID_HANDLE_VALUE) || GetLastError() != ERROR_FILE_NOT_FOUND)
			{
				if(handle)
				{
					FindClose(handle);
				}

				errMsg = CreateSystemErrorMessage(FormatString(L"File \'%s\' should have no streams.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			std::wstring fooStreamName = testFile + L":foo";
			std::wstring barStreamName = testFile + L":bar";

			handle = CreateFileW(fooStreamName.c_str(), fileDesiredAccess, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);

			if(!IsValidHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to create file \'%s\'.", fooStreamName.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!CloseHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to close handle for file \'%s\'.", fooStreamName.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			handle = CreateFileW(barStreamName.c_str(), fileDesiredAccess, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);

			if(!IsValidHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to create file \'%s\'.", barStreamName.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!CloseHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to close handle for file \'%s\'.", barStreamName.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			streams.clear();

			handle = FindFirstStreamW(testFile.c_str(), FindStreamInfoStandard, &findStreamData, 0);

			if(!IsValidHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to find streams for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			do
			{
				streams.push_back(findStreamData);

			} while(FindNextStreamW(handle, &findStreamData));

			if(!FindClose(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to close streams handle for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(streams.size() != 3)
			{
				errMsg = FormatString(
					L"Invalid number of streams (%u) for file \'%s\' - expected 3 streams.",
					static_cast<DWORD>(streams.size()),
					testFile.c_str());

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			bool foundDataStream = false;
			bool foundFooStream = false;
			bool foundBarStream = false;

			for(size_t i = 0; i < streams.size(); ++i)
			{
				if(wcscmp(streams[i].cStreamName, L"::$DATA") == 0)
				{
					foundDataStream = true;
				}
				else if(wcscmp(streams[i].cStreamName, L":foo:$DATA") == 0)
				{
					foundFooStream = true;
				}
				else if(wcscmp(streams[i].cStreamName, L":bar:$DATA") == 0)
				{
					foundBarStream = true;
				}
				else
				{
					errMsg = FormatString(
						L"Unexpected stream \'%s\' for file \'%s\'.",
						streams[i].cStreamName,
						testFile.c_str());

					Assert::Fail(errMsg.c_str(), LINE_INFO());
				}
			}

			if(!foundDataStream || !foundFooStream || !foundBarStream)
			{
				errMsg = FormatString(L"Didn't find expected streams for file \'%s\'.", testFile.c_str());

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!DeleteFileW(testFile.c_str()))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to delete file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			handle = FindFirstStreamW(testFile.c_str(), FindStreamInfoStandard, &findStreamData, 0);

			if((handle && handle != INVALID_HANDLE_VALUE) || GetLastError() != ERROR_FILE_NOT_FOUND)
			{
				if(handle)
				{
					FindClose(handle);
				}

				errMsg = CreateSystemErrorMessage(FormatString(L"File \'%s\' should have no streams.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!CreateDirectoryW(testFile.c_str(), nullptr))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to create directory \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			handle = CreateFileW(fooStreamName.c_str(), fileDesiredAccess, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);

			if(!IsValidHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to create file \'%s\'.", fooStreamName.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!CloseHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to close handle for file \'%s\'.", fooStreamName.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			handle = CreateFileW(barStreamName.c_str(), fileDesiredAccess, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);

			if(!IsValidHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to create file \'%s\'.", barStreamName.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!CloseHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to close handle for file \'%s\'.", barStreamName.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			streams.clear();

			handle = FindFirstStreamW(testFile.c_str(), FindStreamInfoStandard, &findStreamData, 0);

			if(!IsValidHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to find streams for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			do
			{
				streams.push_back(findStreamData);

			} while(FindNextStreamW(handle, &findStreamData));

			if(!FindClose(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to close streams handle for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(streams.size() != 2)
			{
				errMsg = FormatString(
					L"Invalid number of streams (%u) for file \'%s\' - expected 2 streams.",
					static_cast<DWORD>(streams.size()),
					testFile.c_str());

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			//foundDataStream = false;
			foundFooStream = false;
			foundBarStream = false;

			for(size_t i = 0; i < streams.size(); ++i)
			{
				if(wcscmp(streams[i].cStreamName, L":foo:$DATA") == 0)
				{
					foundFooStream = true;
				}
				else if(wcscmp(streams[i].cStreamName, L":bar:$DATA") == 0)
				{
					foundBarStream = true;
				}
				else
				{
					errMsg = FormatString(
						L"Unexpected stream \'%s\' for file \'%s\'.",
						streams[i].cStreamName,
						testFile.c_str());

					Assert::Fail(errMsg.c_str(), LINE_INFO());
				}
			}

			if(!foundFooStream || !foundBarStream)
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Didn't find expected streams for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!RemoveDirectoryW(testFile.c_str()))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to remove directory \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			wchar_t temp[16];

			const int FILE_ARRAY_SIZE = 100;
			const std::wstring fileArryaBaseName = testFile + L":strm";

			for(int createIndex = 0; createIndex < FILE_ARRAY_SIZE; ++createIndex)
			{
				_itow_s(createIndex, temp, 10);

				std::wstring fileName = fileArryaBaseName + temp;

				handle = CreateFileW(fileName.c_str(), fileDesiredAccess, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);

				if(!IsValidHandle(handle))
				{
					errMsg = CreateSystemErrorMessage(FormatString(L"Failed to create file \'%s\'.", fileName.c_str()));

					Assert::Fail(errMsg.c_str(), LINE_INFO());
				}
				else if(!CloseHandle(handle))
				{
					errMsg = CreateSystemErrorMessage(FormatString(L"Failed to close handle for file \'%s\'.", fileName.c_str()));

					Assert::Fail(errMsg.c_str(), LINE_INFO());
				}
			}

			streams.clear();

			handle = FindFirstStreamW(testFile.c_str(), FindStreamInfoStandard, &findStreamData, 0);

			if(!IsValidHandle(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to find streams for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			do
			{
				streams.push_back(findStreamData);

			} while(FindNextStreamW(handle, &findStreamData));

			if(!FindClose(handle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to close streams handle for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(streams.size() != FILE_ARRAY_SIZE + 1)
			{
				errMsg = FormatString(
					L"Invalid number of streams (%u) for file \'%s\' - expected %u streams.",
					static_cast<DWORD>(streams.size()),
					testFile.c_str(),
					static_cast<DWORD>(FILE_ARRAY_SIZE + 1));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			foundDataStream = false;

			std::unique_ptr<bool[]> streamsFound = std::make_unique<bool[]>(FILE_ARRAY_SIZE);
			ZeroMemory(streamsFound.get(), sizeof(bool) * FILE_ARRAY_SIZE);

			const size_t trimStreamBegin = wcslen(L":strm");

			for(size_t i = 0; i < streams.size(); ++i)
			{
				if(wcscmp(streams[i].cStreamName, L"::$DATA") == 0)
				{
					foundDataStream = true;
				}
				else
				{
					if(wcslen(streams[i].cStreamName) <= trimStreamBegin)
					{
						errMsg = FormatString(L"Invalid stream name \'%s\'.", streams[i].cStreamName);

						Assert::Fail(errMsg.c_str(), LINE_INFO());
					}

					wchar_t *indexStrPtr = &streams[i].cStreamName[trimStreamBegin];
					wchar_t *indexStrEndPtr = wcschr(indexStrPtr, L':');

					if(!indexStrEndPtr)
					{
						errMsg = FormatString(L"Invalid stream name \'%s\'.", streams[i].cStreamName);

						Assert::Fail(errMsg.c_str(), LINE_INFO());
					}

					std::wstring indexStr(indexStrPtr, static_cast<size_t>(indexStrEndPtr - indexStrPtr));

					int streamIndex = _wtoi(indexStr.c_str());

					if((streamIndex == 0 && errno == ERANGE) || streamIndex >= FILE_ARRAY_SIZE)
					{
						errMsg = FormatString(L"Unexpected stream name \'%s\'.", streams[i].cStreamName);

						Assert::Fail(errMsg.c_str(), LINE_INFO());
					}

					streamsFound[streamIndex] = true;
				}
			}

			if(!foundDataStream)
			{
				errMsg = FormatString(L"Couldn't find stream \'::$DATA\' for file \'%s\'.", testFile.c_str());

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			for(size_t i = 0; i < FILE_ARRAY_SIZE; ++i)
			{
				if(!streamsFound[i])
				{
					errMsg = FormatString(L"Couldn't find stream \'%u\' for file \'%s\'.",
						static_cast<DWORD>(i),
						testFile.c_str());

					Assert::Fail(errMsg.c_str(), LINE_INFO());
				}
			}

			if(!DeleteFileW(testFile.c_str()))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to delete file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			handle = FindFirstStreamW(testFile.c_str(), FindStreamInfoStandard, &findStreamData, 0);

			if((handle && handle != INVALID_HANDLE_VALUE) || GetLastError() != ERROR_FILE_NOT_FOUND)
			{
				if(handle)
				{
					FindClose(handle);
				}

				errMsg = CreateSystemErrorMessage(FormatString(L"File \'%s\' should have no streams.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			DeleteTestDirectory();
		}

		// This is test 08 in winfstest
		TEST_METHOD(TestSharedFileHandles)
		{
			std::wstring testDir = CreateTestDirectory();
			std::wstring testFile = CombinePath(testDir, L"TestSharedFilehandles.txt");
			std::wstring errMsg;

			HANDLE origHandle = CreateFileW(
				testFile.c_str(),
				GENERIC_WRITE,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr,
				CREATE_NEW,
				FILE_ATTRIBUTE_NORMAL,
				nullptr);

			if(!IsValidHandle(origHandle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to create file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			HANDLE sharedHandle = CreateFileW(
				testFile.c_str(),
				GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL,
				nullptr);

			if(!IsValidHandle(sharedHandle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to open shared handle for file \'%s\'.", testFile.c_str()));

				CloseHandle(origHandle);

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!DeleteFileW(testFile.c_str()))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to set pending delete for file \'%s\'.", testFile.c_str()));

				CloseHandle(sharedHandle);
				CloseHandle(origHandle);

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			HANDLE sharedHandle2 = CreateFileW(
				testFile.c_str(),
				GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL,
				nullptr);

			if(IsValidHandle(sharedHandle2) || GetLastError() != ERROR_ACCESS_DENIED)
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Expected file \'%s\' with pending delete to prevent shared handle from being opened.", testFile.c_str()));

				if(IsValidHandle(sharedHandle2))
				{
					CloseHandle(sharedHandle2);
				}

				CloseHandle(sharedHandle);
				CloseHandle(origHandle);

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!CloseHandle(sharedHandle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to close shared handle for file \'%s\'.", testFile.c_str()));

				CloseHandle(origHandle);

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!CloseHandle(origHandle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to close handle for file \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			origHandle = CreateFileW(
				testFile.c_str(),
				GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL,
				nullptr);

			if(IsValidHandle(sharedHandle2) || GetLastError() != ERROR_FILE_NOT_FOUND)
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"File \'%s\' exists while it was expected to be deleted on close.", testFile.c_str()));

				if(IsValidHandle(origHandle))
				{
					CloseHandle(origHandle);
				}

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!CreateDirectoryW(testFile.c_str(), nullptr))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to create directory \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			std::wstring fooPath = CombinePath(testFile, L"foo.txt");

			origHandle = CreateFileW(
				fooPath.c_str(),
				GENERIC_WRITE,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr,
				CREATE_NEW,
				FILE_ATTRIBUTE_NORMAL,
				nullptr);

			if(!IsValidHandle(origHandle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to create file \'%s\'.", fooPath.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!DeleteFileW(fooPath.c_str()))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to set pending delete for file \'%s\'.", fooPath.c_str()));

				CloseHandle(origHandle);

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			std::wstring filter = CombinePath(testFile, L"*");
			
			WIN32_FIND_DATAW findData;
			std::vector<WIN32_FIND_DATAW> files;
			HANDLE findHandle = FindFirstFileW(filter.c_str(), &findData);

			if(!IsValidHandle(findHandle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to find files for filter \'%s\'.", filter.c_str()));

				CloseHandle(origHandle);

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			do
			{
				files.push_back(findData);

			} while(FindNextFileW(findHandle, &findData));

			FindClose(findHandle);

			if(files.size() != 3)
			{
				errMsg = FormatString(L"FindFiles() expected to return 3 files but instead returned %u files for filter \'%s\'.",
					static_cast<DWORD>(files.size()),
					filter.c_str());

				CloseHandle(origHandle);

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			bool foundCurrentDir = false;
			bool foundParentDir = false;
			bool foundFoo = false;

			for(size_t i = 0; i < files.size(); ++i)
			{
				if(wcscmp(files[i].cFileName, L".") == 0)
				{
					foundCurrentDir = true;
				}
				else if(wcscmp(files[i].cFileName, L"..") == 0)
				{
					foundParentDir = true;
				}
				else if(wcscmp(files[i].cFileName, L"foo.txt") == 0)
				{
					foundFoo = true;
				}
				else
				{
					errMsg = FormatString(L"FindFiles() returned an unexpected file \'%s\' for filter \'%s\'.",
						files[i].cFileName,
						filter.c_str());

					CloseHandle(origHandle);

					Assert::Fail(errMsg.c_str(), LINE_INFO());
				}
			}

			if(!foundCurrentDir || !foundParentDir || !foundFoo)
			{
				errMsg = FormatString(L"FindFiles() did not return the expected list of files for filter \'%s\'.",
					filter.c_str());

				CloseHandle(origHandle);

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!CloseHandle(origHandle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to close handle for file \'%s\'.", fooPath.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			files.clear();

			findHandle = FindFirstFileW(filter.c_str(), &findData);

			if(!IsValidHandle(findHandle))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to find files for filter \'%s\'.", filter.c_str()));

				CloseHandle(origHandle);

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			do
			{
				files.push_back(findData);

			} while(FindNextFileW(findHandle, &findData));

			FindClose(findHandle);

			if(files.size() != 2)
			{
				errMsg = FormatString(L"FindFiles() expected to return 2 files but instead returned %u files for filter \'%s\'.",
					static_cast<DWORD>(files.size()),
					filter.c_str());

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			foundCurrentDir = false;
			foundParentDir = false;
			//foundFoo = false;

			for(size_t i = 0; i < files.size(); ++i)
			{
				if(wcscmp(files[i].cFileName, L".") == 0)
				{
					foundCurrentDir = true;
				}
				else if(wcscmp(files[i].cFileName, L"..") == 0)
				{
					foundParentDir = true;
				}
				else
				{
					errMsg = FormatString(L"FindFiles() returned an unexpected file \'%s\' for filter \'%s\'.",
						files[i].cFileName,
						filter.c_str());

					Assert::Fail(errMsg.c_str(), LINE_INFO());
				}
			}

			if(!foundCurrentDir || !foundParentDir)
			{
				errMsg = FormatString(L"FindFiles() did not return the expected list of files for filter \'%s\'.",
					filter.c_str());

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			if(!RemoveDirectoryW(testFile.c_str()))
			{
				errMsg = CreateSystemErrorMessage(FormatString(L"Failed to remove directory \'%s\'.", testFile.c_str()));

				Assert::Fail(errMsg.c_str(), LINE_INFO());
			}

			DeleteTestDirectory();
		}

		TEST_METHOD(TestFSX)
		{
			std::string testDir = CreateTestDirectoryA();
			std::string testFile = CombinePath(testDir, "testfile");
			std::string goodFile = CombinePath(testDir, "testfile.fsxgood");
			std::string logFile = CombinePath(testDir, "testfile.fsxlog");

			std::string argN("-N");
			std::string argNAmt("5000");

			char pathToExe[MAX_PATH];

			GetModuleFileNameA(nullptr, pathToExe, sizeof(pathToExe));

			std::vector<char*> argv;
			
			argv.push_back(pathToExe);
			argv.push_back(const_cast<char*>(argN.c_str()));
			argv.push_back(const_cast<char*>(argNAmt.c_str()));
			argv.push_back(const_cast<char*>(testFile.c_str()));
			argv.push_back(nullptr);

			Logger::WriteMessage(L"Running FSX...");

			Assert::IsTrue(fsx_main(static_cast<int>(argv.size()) - 1, &argv[0]) == 0, L"FSX failed.", LINE_INFO());

			if(PathFileExistsA(testFile.c_str()))
			{
				if(!DeleteFileA(testFile.c_str()))
				{
					Assert::Fail(L"Failed to delete FSX test file.", LINE_INFO());
				}
			}

			if(PathFileExistsA(goodFile.c_str()))
			{
				if(!DeleteFileA(goodFile.c_str()))
				{
					Assert::Fail(L"Failed to delete FSX good file.", LINE_INFO());
				}
			}

			if(PathFileExistsA(logFile.c_str()))
			{
				if(!DeleteFileA(logFile.c_str()))
				{
					Assert::Fail(L"Failed to delete FSX log file.", LINE_INFO());
				}
			}

			DeleteTestDirectory();
		}
	};
}