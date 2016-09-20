#ifndef UTILS_H_
#define UTILS_H_

#ifdef __cplusplus
#include <string>
#endif
#include <sys/stat.h>
#include "fuse.h"

#ifdef __cplusplus
extern "C" {
#endif
void utf8_to_wchar_buf_old(const char *src, wchar_t *res, int maxlen);
void utf8_to_wchar_buf(const char *src, wchar_t *res, int maxlen);

FILETIME unixTimeToFiletime(time_t t);
time_t filetimeToUnixTime(const FILETIME *ft);
bool is_filetime_set(const FILETIME *ft);

#ifdef __cplusplus
}


std::string wchar_to_utf8_cstr(const wchar_t *str);

std::string unixify(const std::string &str);
std::string extract_file_name(const std::string &str);
std::string extract_dir_name(const std::string &str);

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
	if (stbuf->st_ctim.tv_sec!=0)
		find_data->ftCreationTime=unixTimeToFiletime(stbuf->st_ctim.tv_sec);
	if (stbuf->st_atim.tv_sec!=0)
		find_data->ftLastAccessTime=unixTimeToFiletime(stbuf->st_atim.tv_sec);
	if (stbuf->st_mtim.tv_sec!=0)
		find_data->ftLastWriteTime=unixTimeToFiletime(stbuf->st_mtim.tv_sec);

	//Partial support for read-only files - currently done for a files without write permission only
	if (!(stbuf->st_mode&0222))
		find_data->dwFileAttributes|=FILE_ATTRIBUTE_READONLY;
	//TODO: add full support for read-only files - try to derive it from file's owner?
	std::string fname=extract_file_name(name);
	if (!fname.empty() && fname.at(0)=='.') //UNIX hidden files
		find_data->dwFileAttributes|=FILE_ATTRIBUTE_HIDDEN;
}

#endif // __cplusplus
#endif // UTILS_H_
