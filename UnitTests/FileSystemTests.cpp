#include "stdafx.h"
#include "CppUnitTest.h"

#include <Windows.h>
#include <Shlwapi.h>
#include <string>
#include <cassert>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

#define FILESYSTEM_ROOT L"M:\\"
#define FILESYSTEM_TESTDIR L"M:\\MirrorUnitTests"

const wchar_t* GetErrorCodeStr(const DWORD errCode);

#define USE_MIRROR 1

namespace UnitTests
{		
	TEST_CLASS(FileSystemTests)
	{
	private:
		std::wstring GetTempPathStr()
		{
			WCHAR path[MAX_PATH];

			GetTempPathW(MAX_PATH, path);

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

		std::wstring CreateTestDirectory()
		{
#if USE_MIRROR
			std::wstring testDir = FILESYSTEM_TESTDIR;
#else
			std::wstring testDir = CombinePath(GetTempPathStr(), L"MirrorUnitTests");
#endif

			if(!PathFileExistsW(testDir.c_str()))
			{
				if(!CreateDirectoryW(testDir.c_str(), NULL))
				{
					std::wstring errMsg = CreateSystemErrorMessage(FormatString(L"Failed to CreateDirectoryW for directory \'%s\'.", testDir.c_str()));

					Assert::IsTrue(false, errMsg.c_str(), LINE_INFO());
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

					Assert::IsTrue(false, errMsg.c_str(), LINE_INFO());
				}
			}
		}

		std::wstring FormatString(const wchar_t *str, ...)
		{
			size_t size = 256;
			wchar_t *buf = (wchar_t*)malloc(size * sizeof(wchar_t));
			int written = 0;

			va_list args;
			va_start(args, str);

			while((written = _vsnwprintf_s(buf, size - 1, size - 1, str, args)) == -1)
			{
				size *= 2;
				buf = (wchar_t*)realloc(buf, size * sizeof(wchar_t));

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

		std::wstring GetLastErrorStr(const DWORD lastErr)
		{
			LPWSTR buffer = NULL;

			// we force english because the error messages are for developers and english is the common language
			// developers of all countries use to communicate
			DWORD errCode = FormatMessageW(
				FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
				NULL,
				lastErr,
				MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
				(LPWSTR)&buffer,
				0,
				NULL);

			if(errCode == 0)
			{
				if(GetLastError() == ERROR_RESOURCE_LANG_NOT_FOUND)
				{
					errCode = FormatMessageW(
						FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
						NULL,
						lastErr,
						0,
						(LPWSTR)&buffer,
						0,
						NULL);
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

			HANDLE handle = CreateFileW(testFile.c_str(), fileDesiredAccess, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

			if(!handle)
			{
				std::wstring errMsg = CreateSystemErrorMessage(FormatString(L"Failed to create file \'%s\'.", testFile.c_str()));

				Assert::IsTrue(false, errMsg.c_str(), LINE_INFO());
			}

			BY_HANDLE_FILE_INFORMATION fileInfo;
			ZeroMemory(&fileInfo, sizeof(fileInfo));

			if(!GetFileInformationByHandle(handle, &fileInfo))
			{
				std::wstring errMsg = CreateSystemErrorMessage(FormatString(L"Failed to GetFileInformationByHandle for file \'%s\'.", testFile.c_str()));

				Assert::IsTrue(false, errMsg.c_str(), LINE_INFO());
			}

			FILE_BASIC_INFO basicInfo;
			basicInfo.ChangeTime = *(LARGE_INTEGER*)&fileInfo.ftLastWriteTime;
			basicInfo.CreationTime = *(LARGE_INTEGER*)&fileInfo.ftCreationTime;
			basicInfo.FileAttributes = fileInfo.dwFileAttributes;
			basicInfo.LastAccessTime = *(LARGE_INTEGER*)&fileInfo.ftLastAccessTime;
			basicInfo.LastWriteTime = *(LARGE_INTEGER*)&fileInfo.ftLastWriteTime;

			basicInfo.CreationTime.QuadPart -= 500;

			if(!SetFileInformationByHandle(handle, FileBasicInfo, &basicInfo, sizeof(basicInfo)))
			{
				std::wstring errMsg = CreateSystemErrorMessage(FormatString(L"Failed to SetFileInformationByHandle for file \'%s\'.", testFile.c_str()));

				Assert::IsTrue(false, errMsg.c_str(), LINE_INFO());
			}

			BY_HANDLE_FILE_INFORMATION fileInfo2;
			ZeroMemory(&fileInfo2, sizeof(fileInfo2));

			if(!GetFileInformationByHandle(handle, &fileInfo2))
			{
				std::wstring errMsg = CreateSystemErrorMessage(FormatString(L"Failed to GetFileInformationByHandle for modified file \'%s\'.", testFile.c_str()));

				Assert::IsTrue(false, errMsg.c_str(), LINE_INFO());
			}

			Assert::IsTrue(((LARGE_INTEGER*)&fileInfo2.ftCreationTime)->QuadPart == basicInfo.CreationTime.QuadPart,
				L"Failed to properly set modified file creation time.",
				LINE_INFO());

			if(!CloseHandle(handle))
			{
				std::wstring errMsg = CreateSystemErrorMessage(FormatString(L"Failed to CloseHandle for file \'%s\'.", testFile.c_str()));

				Assert::IsTrue(false, errMsg.c_str(), LINE_INFO());
			}

			if(!DeleteFileW(testFile.c_str()))
			{
				std::wstring errMsg = CreateSystemErrorMessage(FormatString(L"Failed DeleteFileW for file \'%s\'.", testFile.c_str()));

				Assert::IsTrue(false, errMsg.c_str(), LINE_INFO());
			}

			DeleteTestDirectory();
		}
	};
}