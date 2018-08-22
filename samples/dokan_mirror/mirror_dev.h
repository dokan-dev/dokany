#pragma once
#include "../../dokan/dokan.h"

//#define WIN10_ENABLE_LONG_PATH
#ifdef WIN10_ENABLE_LONG_PATH
//dirty but should be enough
#define DOKAN_MAX_PATH 32768
#else
#define DOKAN_MAX_PATH MAX_PATH
#endif // DEBUG

#define MirrorCheckFlag(val, flag)                                             \
  if (val & flag) {                                                            \
    DbgPrint(L"\t" L#flag L"\n");                                              \
  }

extern BOOL g_UseStdErr;
extern BOOL g_DebugMode;

enum MirrorDevFileType
{
  /**
  * Expose the drive as a raw file in the filesystem
  */
  MirrorDevTypeRawFile=0,
  /**
  * Expose the drive as a .vhd file in the filesystem
  */
  MirrorDevTypeVHD,
  /**
  * Expose the drive as a .vhdx file in the filesystem
  */
  MirrorDevTypeVHDX
};


#ifdef __cplusplus
extern "C" {
#endif

  int MirrorDevInit(enum MirrorDevFileType type,
    LPWSTR PhysicalDrive, PDOKAN_OPTIONS DokanOptions, PDOKAN_OPERATIONS DokanOperations);
  void MirrorDevTeardown();
  void DbgPrint(LPCWSTR format, ...);

  static wchar_t *g_MirrorDevDokanPathPrefix = L"\\MIRROR";
  static size_t g_MirrorDevDokanPathPrefixLength = 8;
  static wchar_t *g_MirrorDevDokanVhdPostfix = L".VHD";
  static size_t g_MirrorDevDokanVhdPostfixLength = 4;
  static wchar_t *g_MirrorDevDokanVhdxPostfix = L".VHDX";
  static size_t g_MirrorDevDokanVhdxPostfixLength = 5;

#ifdef __cplusplus
}
#endif
