#ifndef _UTILS_H_
#define _UTILS_H_

#include <string>
#include <sys/stat.h>
#include "fuse.h"

/*#ifdef _MSC_VER
#define DLLLOCAL
#else
#define DLLLOCAL __attribute__ ((visibility("hidden")))
#endif*/

void utf8_to_wchar_buf_old(const char *src, wchar_t *res, int maxlen);
void utf8_to_wchar_buf(const char *src, wchar_t *res, int maxlen);
std::string wchar_to_utf8_cstr(const wchar_t *str);

std::string unixify(const std::string &str);
std::string extract_file_name(const std::string &str);
std::string extract_dir_name(const std::string &str);

FILETIME unixTimeToFiletime(time_t t);
time_t filetimeToUnixTime(const FILETIME *ft);
bool is_filetime_set(const FILETIME *ft);

template<class T> void convertStatlikeBuf(const struct FUSE_STAT *stbuf, const std::string &name, 
										  T * find_data)
{
	if (stbuf==NULL) return;

	if ((stbuf->st_mode&S_IFDIR)==S_IFDIR)
		find_data->dwFileAttributes=FILE_ATTRIBUTE_DIRECTORY;
	else
		find_data->dwFileAttributes=FILE_ATTRIBUTE_NORMAL;

#ifndef WIDE_OFF_T
	find_data->nFileSizeLow=stbuf->st_size;
#else
	//Support 64 sizes
	find_data->nFileSizeLow=(DWORD) stbuf->st_size;
	find_data->nFileSizeHigh=stbuf->st_size>>32;
#endif
	if (stbuf->st_ctime!=0)
		find_data->ftCreationTime=unixTimeToFiletime(stbuf->st_ctime);
	if (stbuf->st_atime!=0)
		find_data->ftLastAccessTime=unixTimeToFiletime(stbuf->st_atime);
	if (stbuf->st_mtime!=0)
		find_data->ftLastWriteTime=unixTimeToFiletime(stbuf->st_mtime);

	//TODO: add support for read-only files - try to derive it from file's owner?
	std::string fname=extract_file_name(name);
	if (!fname.empty() && fname.at(0)=='.') //UNIX hidden files
		find_data->dwFileAttributes|=FILE_ATTRIBUTE_HIDDEN;
}

#endif
