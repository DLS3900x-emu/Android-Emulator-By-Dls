// Minimal POSIX emulation of the Win32 calls used by the disk/boot code (tests only).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
typedef void* HANDLE; typedef int BOOL; typedef unsigned long DWORD; typedef uint32_t UINT32; typedef uint64_t UINT64;
typedef unsigned int UINT; typedef long LONG; typedef int64_t LONGLONG;
#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define CREATE_ALWAYS 2
#define FILE_FLAG_RANDOM_ACCESS 0
#define FILE_FLAG_SEQUENTIAL_SCAN 0
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define ERROR_HANDLE_EOF 38
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FSCTL_SET_SPARSE 0
#define CP_UTF8 65001
#define MEM_COMMIT 0
#define MEM_RESERVE 0
#define MEM_RELEASE 0
#define PAGE_READWRITE 0
typedef union { struct { DWORD LowPart; LONG HighPart; }; LONGLONG QuadPart; } LARGE_INTEGER;
typedef struct { uintptr_t Internal, InternalHigh; DWORD Offset, OffsetHigh; HANDLE hEvent; } OVERLAPPED;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct { DWORD dwFileAttributes; FILETIME a,b,ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow; } WIN32_FILE_ATTRIBUTE_DATA;
enum { GetFileExInfoStandard };
inline thread_local DWORD g_err;
inline DWORD GetLastError(){ return g_err; }
inline std::string U8(const wchar_t* w){ std::string s; for(;*w;++w) s.push_back(*w==L'\\' ? '/' : (char)*w); return s; }
inline HANDLE CreateFileW(const wchar_t* p, DWORD acc, DWORD, void*, DWORD disp, DWORD, void*){
  int fl = (acc & GENERIC_WRITE) ? O_RDWR : O_RDONLY;
  if (disp==CREATE_ALWAYS) fl |= O_CREAT|O_TRUNC; if (disp==OPEN_ALWAYS) fl|=O_CREAT;
  int fd = open(U8(p).c_str(), fl, 0644); if (fd<0){ g_err=errno; return INVALID_HANDLE_VALUE;} return (HANDLE)(intptr_t)(fd+1); }
inline int FD(HANDLE h){ return (int)(intptr_t)h - 1; }
inline BOOL CloseHandle(HANDLE h){ return close(FD(h))==0; }
inline BOOL ReadFile(HANDLE h, void* b, DWORD n, DWORD* got, OVERLAPPED* ov){
  ssize_t r = ov ? pread(FD(h), b, n, ((off_t)ov->OffsetHigh<<32)|ov->Offset) : read(FD(h), b, n);
  if (r<0) return FALSE; *got=(DWORD)r; return TRUE; }
inline BOOL WriteFile(HANDLE h, const void* b, DWORD n, DWORD* got, OVERLAPPED* ov){
  ssize_t r = ov ? pwrite(FD(h), b, n, ((off_t)ov->OffsetHigh<<32)|ov->Offset) : write(FD(h), b, n);
  if (r<0) return FALSE; *got=(DWORD)r; return TRUE; }
inline BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER d, LARGE_INTEGER*, DWORD m){ return lseek(FD(h), d.QuadPart, m==FILE_BEGIN?SEEK_SET:SEEK_CUR)>=0; }
inline BOOL SetEndOfFile(HANDLE h){ off_t p=lseek(FD(h),0,SEEK_CUR); return ftruncate(FD(h),p)==0; }
inline BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER* s){ struct stat st; fstat(FD(h),&st); s->QuadPart=st.st_size; return TRUE; }
inline BOOL FlushFileBuffers(HANDLE h){ return fsync(FD(h))==0; }
inline BOOL DeviceIoControl(HANDLE,DWORD,void*,DWORD,void*,DWORD,DWORD*,void*){ return TRUE; }
inline DWORD GetFileAttributesW(const wchar_t* p){ struct stat st; if(stat(U8(p).c_str(),&st)) return INVALID_FILE_ATTRIBUTES; return S_ISDIR(st.st_mode)?FILE_ATTRIBUTE_DIRECTORY:0; }
inline BOOL GetFileAttributesExW(const wchar_t* p, int, WIN32_FILE_ATTRIBUTE_DATA* d){ struct stat st; if(stat(U8(p).c_str(),&st)) return FALSE;
  memset(d,0,sizeof(*d)); d->nFileSizeLow=(DWORD)(st.st_size&0xffffffff); d->nFileSizeHigh=(DWORD)(st.st_size>>32);
  uint64_t t=(uint64_t)st.st_mtim.tv_sec*10000000ull+st.st_mtim.tv_nsec/100; d->ftLastWriteTime.dwLowDateTime=(DWORD)t; d->ftLastWriteTime.dwHighDateTime=(DWORD)(t>>32); return TRUE; }
inline BOOL CreateDirectoryW(const wchar_t* p, void*){ return mkdir(U8(p).c_str(),0755)==0; }
inline BOOL MoveFileW(const wchar_t* a, const wchar_t* b){ return rename(U8(a).c_str(),U8(b).c_str())==0; }
inline BOOL DeleteFileW(const wchar_t* a){ return unlink(U8(a).c_str())==0; }
inline int MultiByteToWideChar(int,int,const char* s,int n,wchar_t* w,int cap){ if(!w) return n; for(int i=0;i<n&&i<cap;++i) w[i]=(unsigned char)s[i]; return n; }
inline FILE* _wfopen(const wchar_t* p, const wchar_t* m){ return fopen(U8(p).c_str(), U8(m).c_str()); }
#define _fseeki64 fseeko
#define _ftelli64 ftello
#define MemoryBarrier() __sync_synchronize()
inline void* VirtualAlloc(void*, size_t n, int, int){ return calloc(1,n); }
inline BOOL VirtualFree(void* p, size_t, int){ free(p); return TRUE; }
inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* f){ f->QuadPart=1000000000; return TRUE; }
inline BOOL QueryPerformanceCounter(LARGE_INTEGER* c){ timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); c->QuadPart=ts.tv_sec*1000000000ll+ts.tv_nsec; return TRUE; }
