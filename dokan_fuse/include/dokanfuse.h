#ifndef DOKANFUSE_H_
#define DOKANFUSE_H_

#include <string>

#define FUSE_THREAD_COUNT 10
#define DOKAN_DLL L"dokan" DOKAN_MAJOR_API_VERSION L".dll"

// Cygwin64 is LP64 while Windows 64bit is LLP64.
// This why we define macros in the style of inttypes.h for printing DWORDs and ULONGs which are fixed to 32 bit.
#ifdef __LP64__
// DWORD and ULONG are unsigned int
# define PRIxDWORD "%x"
# define PRIuDWORD "%u"
# define PRIxULONG "%x"
# define PRIuULONG "%u"
#else
// DWORD and ULONG are unsigned long int
# define PRIxDWORD "%lx"
# define PRIuDWORD "%lu"
# define PRIxULONG "%lx"
# define PRIuULONG "%lu"
#endif

struct fuse_config
{
  unsigned int umask;
  unsigned int fileumask, dirumask;
  const char *fsname, *volname, *uncname;
  int help;
  int debug;
  int mountManager;
  int readonly;
  int setsignals;
  unsigned int timeoutInSec;
  int removableDrive;
  int networkDrive;
  unsigned long allocationUnitSize;
  unsigned long sectorSize;
};

struct fuse_session
{
	fuse_chan *ch;
};

struct fuse_chan
{
	fuse_chan() = default;
	~fuse_chan();
	fuse_chan(fuse_chan &other) = delete;
	fuse_chan &operator=(const fuse_chan &other) = delete;

	//This method dynamically loads DOKAN functions
	bool init();

	typedef VOID (__stdcall *DokanInitType)();
	typedef VOID(__stdcall *DokanShutdownType)();
	typedef int (__stdcall *DokanMainType)(PDOKAN_OPTIONS,PDOKAN_OPERATIONS);
	typedef BOOL (__stdcall *DokanUnmountType)(WCHAR DriveLetter);
	typedef BOOL (__stdcall *DokanRemoveMountPointType)(LPCWSTR MountPoint);
	DokanInitType ResolvedDokanInit = nullptr;
	DokanShutdownType ResolvedDokanShutdown = nullptr;
	DokanMainType ResolvedDokanMain = nullptr;
	DokanUnmountType ResolvedDokanUnmount = nullptr;
	DokanRemoveMountPointType ResolvedDokanRemoveMountPoint = nullptr;

	std::string mountpoint;
private:
	HMODULE dokanDll = nullptr;
};

struct fuse
{
	bool within_loop;
	std::unique_ptr<fuse_chan> ch;
	fuse_session sess;
	fuse_config conf;

	struct fuse_operations ops;
	void *user_data;

	fuse() : within_loop(), user_data()
	{
		memset(&conf,0,sizeof(fuse_config));
		memset(&sess, 0, sizeof(fuse_session));
		memset(&ops, 0, sizeof(fuse_operations));
	}
};

#endif //DOKANFUSE_H_
