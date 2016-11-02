#include <stdafx.h>

/*
 * sys/mman.c
 * mman-win32
 *
 * https://code.google.com/p/mman-win32/
 * MIT License
 */

#include <windows.h>
#include <errno.h>
#include <io.h>

#include "mman.h"

#ifndef FILE_MAP_EXECUTE
#define FILE_MAP_EXECUTE    0x0020
#endif /* FILE_MAP_EXECUTE */

int __acrt_errno_map_os_error(const DWORD err)
{
    if (err == 0)
        return 0;
    
	switch(err)
	{
		// https://github.com/Feuerlabs/fnotify/blob/master/c_src/dosmap.c
		case   1: return EINVAL;
		case   2: return ENOENT;
		case   3: return ENOENT;
		case   4: return EMFILE;
		case   5: return EACCES;
		case   6: return EBADF ;
		case   7: return ENOMEM;
		case   8: return ENOMEM;
		case   9: return ENOMEM;
		case  10: return E2BIG ;
		case  11: return ENOEXEC;
		case  12: return EINVAL;
		case  13: return EINVAL;
		case  14: return EINVAL;
		case  15: return ENOENT;
		case  16: return EACCES;
		case  17: return EXDEV ;
		case  18: return ENOENT;
		case  19: return EACCES;
		case  20: return EACCES;
		case  21: return EACCES;
		case  22: return EACCES;
		case  23: return EACCES;
		case  24: return EACCES;
		case  25: return EACCES;
		case  26: return EACCES;
		case  27: return EACCES;
		case  28: return EACCES;
		case  29: return EACCES;
		case  30: return EACCES;
		case  31: return EACCES;
		case  32: return EACCES;
		case  33: return EACCES;
		case  34: return EACCES;
		case  35: return EACCES;
		case  36: return EACCES;
		case  37: return EINVAL;
		case  38: return EINVAL;
		case  39: return EINVAL;
		case  40: return EINVAL;
		case  41: return EINVAL;
		case  42: return EINVAL;
		case  43: return EINVAL;
		case  44: return EINVAL;
		case  45: return EINVAL;
		case  46: return EINVAL;
		case  47: return EINVAL;
		case  48: return EINVAL;
		case  49: return EINVAL;
		case  50: return EINVAL;
		case  51: return EINVAL;
		case  52: return EINVAL;
		case  53: return ENOENT;
		case  54: return EINVAL;
		case  55: return EINVAL;
		case  56: return EINVAL;
		case  57: return EINVAL;
		case  58: return EINVAL;
		case  59: return EINVAL;
		case  60: return EINVAL;
		case  61: return EINVAL;
		case  62: return EINVAL;
		case  63: return EINVAL;
		case  64: return EINVAL;
		case  65: return EACCES;
		case  66: return EINVAL;
		case  67: return ENOENT;
		case  68: return EINVAL;
		case  69: return EINVAL;
		case  70: return EINVAL;
		case  71: return EINVAL;
		case  72: return EINVAL;
		case  73: return EINVAL;
		case  74: return EINVAL;
		case  75: return EINVAL;
		case  76: return EINVAL;
		case  77: return EINVAL;
		case  78: return EINVAL;
		case  79: return EINVAL;
		case  80: return EEXIST;
		case  81: return EINVAL;
		case  82: return EACCES;
		case  83: return EACCES;
		case  84: return EINVAL;
		case  85: return EINVAL;
		case  86: return EINVAL;
		case  87: return EINVAL;
		case  88: return EINVAL;
		case  89: return EAGAIN;
		case  90: return EINVAL;
		case  91: return EINVAL;
		case  92: return EINVAL;
		case  93: return EINVAL;
		case  94: return EINVAL;
		case  95: return EINVAL;
		case  96: return EINVAL;
		case  97: return EINVAL;
		case  98: return EINVAL;
		case  99: return EINVAL;
		case 100: return EINVAL;
		case 101: return EINVAL;
		case 102: return EINVAL;
		case 103: return EINVAL;
		case 104: return EINVAL;
		case 105: return EINVAL;
		case 106: return EINVAL;
		case 107: return EINVAL;
		case 108: return EACCES;
		case 109: return EPIPE ;
		case 110: return EINVAL;
		case 111: return EINVAL;
		case 112: return ENOSPC;
		case 113: return EINVAL;
		case 114: return EBADF ;
		case 115: return EINVAL;
		case 116: return EINVAL;
		case 117: return EINVAL;
		case 118: return EINVAL;
		case 119: return EINVAL;
		case 120: return EINVAL;
		case 121: return EINVAL;
		case 122: return EINVAL;
		case 123: return EINVAL;
		case 124: return EINVAL;
		case 125: return EINVAL;
		case 126: return EINVAL;
		case 127: return EINVAL;
		case 128: return ECHILD;
		case 129: return ECHILD;
		case 130: return EBADF ;
		case 131: return EINVAL;
		case 132: return EACCES;
		case 133: return EINVAL;
		case 134: return EINVAL;
		case 135: return EINVAL;
		case 136: return EINVAL;
		case 137: return EINVAL;
		case 138: return EINVAL;
		case 139: return EINVAL;
		case 140: return EINVAL;
		case 141: return EINVAL;
		case 142: return EINVAL;
		case 143: return EINVAL;
		case 144: return EINVAL;
		case 145: return ENOTEMPTY;
		case 146: return EINVAL;
		case 147: return EINVAL;
		case 148: return EINVAL;
		case 149: return EINVAL;
		case 150: return EINVAL;
		case 151: return EINVAL;
		case 152: return EINVAL;
		case 153: return EINVAL;
		case 154: return EINVAL;
		case 155: return EINVAL;
		case 156: return EINVAL;
		case 157: return EINVAL;
		case 158: return EACCES;
		case 159: return EINVAL;
		case 160: return EINVAL;
		case 161: return ENOENT;
		case 162: return EINVAL;
		case 163: return EINVAL;
		case 164: return EAGAIN;
		case 165: return EINVAL;
		case 166: return EINVAL;
		case 167: return EACCES;
		case 168: return EINVAL;
		case 169: return EINVAL;
		case 170: return EINVAL;
		case 171: return EINVAL;
		case 172: return EINVAL;
		case 173: return EINVAL;
		case 174: return EINVAL;
		case 175: return EINVAL;
		case 176: return EINVAL;
		case 177: return EINVAL;
		case 178: return EINVAL;
		case 179: return EINVAL;
		case 180: return EINVAL;
		case 181: return EINVAL;
		case 182: return EINVAL;
		case 183: return EEXIST;
		case 184: return EINVAL;
		case 185: return EINVAL;
		case 186: return EINVAL;
		case 187: return EINVAL;
		case 188: return ENOEXEC;
		case 189: return ENOEXEC;
		case 190: return ENOEXEC;
		case 191: return ENOEXEC;
		case 192: return ENOEXEC;
		case 193: return ENOEXEC;
		case 194: return ENOEXEC;
		case 195: return ENOEXEC;
		case 196: return ENOEXEC;
		case 197: return ENOEXEC;
		case 198: return ENOEXEC;
		case 199: return ENOEXEC;
		case 200: return ENOEXEC;
		case 201: return ENOEXEC;
		case 202: return ENOEXEC;
		case 203: return EINVAL;
		case 204: return EINVAL;
		case 205: return EINVAL;
		case 206: return ENOENT;
		case 207: return EINVAL;
		case 208: return EINVAL;
		case 209: return EINVAL;
		case 210: return EINVAL;
		case 211: return EINVAL;
		case 212: return EINVAL;
		case 213: return EINVAL;
		case 214: return EINVAL;
		case 215: return EAGAIN;
	}

	return EINVAL;
}

static int __map_mman_error(const DWORD err, const int deferr)
{
	return __acrt_errno_map_os_error(err);
}

static DWORD __map_mmap_prot_page(const int prot)
{
    DWORD protect = 0;
    
    if (prot == PROT_NONE)
        return protect;
        
    if ((prot & PROT_EXEC) != 0)
    {
        protect = ((prot & PROT_WRITE) != 0) ? 
                    PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    }
    else
    {
        protect = ((prot & PROT_WRITE) != 0) ?
                    PAGE_READWRITE : PAGE_READONLY;
    }
    
    return protect;
}

static DWORD __map_mmap_prot_file(const int prot)
{
    DWORD desiredAccess = 0;
    
    if (prot == PROT_NONE)
        return desiredAccess;
        
    if ((prot & PROT_READ) != 0)
        desiredAccess |= FILE_MAP_READ;
    if ((prot & PROT_WRITE) != 0)
        desiredAccess |= FILE_MAP_WRITE;
    if ((prot & PROT_EXEC) != 0)
        desiredAccess |= FILE_MAP_EXECUTE;
    
    return desiredAccess;
}

void* mmap(void *addr, size_t len, int prot, int flags, int fildes, off_t off)
{
    HANDLE fm, h;
    
    void * map = MAP_FAILED;
    
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4293)
#endif

    const DWORD dwFileOffsetLow = (sizeof(off_t) <= sizeof(DWORD)) ? 
                    (DWORD)off : (DWORD)(off & 0xFFFFFFFFL);
    const DWORD dwFileOffsetHigh = (sizeof(off_t) <= sizeof(DWORD)) ?
                    (DWORD)0 : (DWORD)((off >> 32) & 0xFFFFFFFFL);
    const DWORD protect = __map_mmap_prot_page(prot);
    const DWORD desiredAccess = __map_mmap_prot_file(prot);

    const off_t maxSize = off + (off_t)len;

    const DWORD dwMaxSizeLow = (sizeof(off_t) <= sizeof(DWORD)) ? 
                    (DWORD)maxSize : (DWORD)(maxSize & 0xFFFFFFFFL);
    const DWORD dwMaxSizeHigh = (sizeof(off_t) <= sizeof(DWORD)) ?
                    (DWORD)0 : (DWORD)((maxSize >> 32) & 0xFFFFFFFFL);

#ifdef _MSC_VER
#pragma warning(pop)
#endif

    errno = 0;
    
    if (len == 0 
        /* Unsupported flag combinations */
        || (flags & MAP_FIXED) != 0
        /* Usupported protection combinations */
        || prot == PROT_EXEC)
    {
        errno = EINVAL;
        return MAP_FAILED;
    }
    
    h = ((flags & MAP_ANONYMOUS) == 0) ? 
                    (HANDLE)_get_osfhandle(fildes) : INVALID_HANDLE_VALUE;

    if ((flags & MAP_ANONYMOUS) == 0 && h == INVALID_HANDLE_VALUE)
    {
        errno = EBADF;
        return MAP_FAILED;
    }

    fm = CreateFileMapping(h, NULL, protect, dwMaxSizeHigh, dwMaxSizeLow, NULL);

    if (fm == NULL)
    {
        errno = __map_mman_error(GetLastError(), EPERM);
        return MAP_FAILED;
    }
  
    map = MapViewOfFile(fm, desiredAccess, dwFileOffsetHigh, dwFileOffsetLow, len);

    CloseHandle(fm);
  
    if (map == NULL)
    {
        errno = __map_mman_error(GetLastError(), EPERM);
        return MAP_FAILED;
    }

    return map;
}

int munmap(void *addr, size_t len)
{
    if (UnmapViewOfFile(addr))
        return 0;
        
    errno =  __map_mman_error(GetLastError(), EPERM);
    
    return -1;
}

int mprotect(void *addr, size_t len, int prot)
{
    DWORD newProtect = __map_mmap_prot_page(prot);
    DWORD oldProtect = 0;
    
    if (VirtualProtect(addr, len, newProtect, &oldProtect))
        return 0;
    
    errno =  __map_mman_error(GetLastError(), EPERM);
    
    return -1;
}

int msync(void *addr, size_t len, int flags)
{
    if (FlushViewOfFile(addr, len))
        return 0;
    
    errno =  __map_mman_error(GetLastError(), EPERM);
    
    return -1;
}

int mlock(const void *addr, size_t len)
{
    if (VirtualLock((LPVOID)addr, len))
        return 0;
        
    errno =  __map_mman_error(GetLastError(), EPERM);
    
    return -1;
}

int munlock(const void *addr, size_t len)
{
    if (VirtualUnlock((LPVOID)addr, len))
        return 0;
        
    errno =  __map_mman_error(GetLastError(), EPERM);
    
    return -1;
}
