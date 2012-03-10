/*
 * Windows support routines for PhysicsFS.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon, and made sane by Gregory S. Read.
 */

#define __PHYSICSFS_INTERNAL__
#include "physfs_platforms.h"

#ifdef PHYSFS_PLATFORM_WINDOWS

/* Forcibly disable UNICODE macro, since we manage this ourselves. */
#ifdef UNICODE
#undef UNICODE
#endif

#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
#include <userenv.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>

#include "physfs_internal.h"

#define LOWORDER_UINT64(pos) ((PHYSFS_uint32) (pos & 0xFFFFFFFF))
#define HIGHORDER_UINT64(pos) ((PHYSFS_uint32) ((pos >> 32) & 0xFFFFFFFF))

/*
 * Users without the platform SDK don't have this defined.  The original docs
 *  for SetFilePointer() just said to compare with 0xFFFFFFFF, so this should
 *  work as desired.
 */
#define PHYSFS_INVALID_SET_FILE_POINTER  0xFFFFFFFF

/* just in case... */
#define PHYSFS_INVALID_FILE_ATTRIBUTES   0xFFFFFFFF

/* Not defined before the Vista SDK. */
#define PHYSFS_IO_REPARSE_TAG_SYMLINK    0xA000000C


#define UTF8_TO_UNICODE_STACK_MACRO(w_assignto, str) { \
    if (str == NULL) \
        w_assignto = NULL; \
    else { \
        const PHYSFS_uint64 len = (PHYSFS_uint64) ((strlen(str) + 1) * 2); \
        w_assignto = (WCHAR *) __PHYSFS_smallAlloc(len); \
        if (w_assignto != NULL) \
            PHYSFS_utf8ToUcs2(str, (PHYSFS_uint16 *) w_assignto, len); \
    } \
} \

#ifndef _MSC_VER
#define _snprintf snprintf
#endif

/* !!! FIXME: this is wrong for UTF-16. */
static PHYSFS_uint64 wStrLen(const WCHAR *wstr)
{
    PHYSFS_uint64 len = 0;
    while (*(wstr++))
        len++;
    return len;
} /* wStrLen */

static char *unicodeToUtf8Heap(const WCHAR *w_str)
{
    char *retval = NULL;
    if (w_str != NULL)
    {
        void *ptr = NULL;
        const PHYSFS_uint64 len = (wStrLen(w_str) * 4) + 1;
        retval = allocator.Malloc(len);
        BAIL_IF_MACRO(retval == NULL, ERR_OUT_OF_MEMORY, NULL);
        /* !!! FIXME: utf-16. */
        PHYSFS_utf8FromUcs2((const PHYSFS_uint16 *) w_str, retval, len);
        ptr = allocator.Realloc(retval, strlen(retval) + 1); /* shrink. */
        if (ptr != NULL)
            retval = (char *) ptr;
    } /* if */
    return retval;
} /* unicodeToUtf8Heap */

/* !!! FIXME: do we really need readonly? If not, do we need this struct? */
typedef struct
{
    HANDLE handle;
    int readonly;
} WinApiFile;


const char *__PHYSFS_platformDirSeparator = "\\";
static char *userDir = NULL;
static HANDLE libUserEnv = NULL;


/*
 * Figure out what the last failing Windows API call was, and
 *  generate a human-readable string for the error message.
 *
 * The return value is a static buffer that is overwritten with
 *  each call to this function.
 */
static const char *winApiStrErrorByNum(const DWORD err)
{
    static char utf8buf[255];
    WCHAR msgbuf[255];
    WCHAR *ptr;
    DWORD rc = FormatMessageW(
                    FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                    NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                    msgbuf, __PHYSFS_ARRAYLEN(msgbuf),
                    NULL);

    if (rc == 0)
        msgbuf[0] = '\0';  /* oh well. */

    /* chop off newlines. */
    for (ptr = msgbuf; *ptr; ptr++)
    {
        if ((*ptr == '\n') || (*ptr == '\r'))
        {
            *ptr = '\0';
            break;
        } /* if */
    } /* for */

    /* may truncate, but oh well. */
    PHYSFS_utf8FromUcs2((PHYSFS_uint16 *) msgbuf, utf8buf, sizeof (utf8buf));
    return ((const char *) utf8buf);
} /* winApiStrErrorByNum */

static inline const char *winApiStrError(void)
{
    return winApiStrErrorByNum(GetLastError());
} /* winApiStrError */

/*
 * On success, module-scope variable (userDir) will have a pointer to
 *  a malloc()'d string of the user's profile dir, and a non-zero value is
 *  returned. If we can't determine the profile dir, (userDir) will
 *  be NULL, and zero is returned.
 */
static int determineUserDir(void)
{
    typedef BOOL (WINAPI *fnGetUserProfDirW)(HANDLE, LPWSTR, LPDWORD);
    fnGetUserProfDirW pGetDir = NULL;

    HANDLE accessToken = NULL;       /* Security handle to process */

    if (userDir != NULL)
        return 1;  /* already good to go. */

    pGetDir = (fnGetUserProfDirW)
        GetProcAddress(libUserEnv, "GetUserProfileDirectoryW");
    BAIL_IF_MACRO(pGetDir == NULL, winApiStrError(), 0);

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &accessToken))
        BAIL_MACRO(winApiStrError(), 0);
    else
    {
        DWORD psize = 0;
        WCHAR dummy = 0;
        LPWSTR wstr = NULL;
        BOOL rc = 0;

        /*
         * Should fail. Will write the size of the profile path in
         *  psize. Also note that the second parameter can't be
         *  NULL or the function fails.
         */
    	rc = pGetDir(accessToken, &dummy, &psize);
        assert(!rc);  /* !!! FIXME: handle this gracefully. */
        (void) rc;

        /* Allocate memory for the profile directory */
        wstr = (LPWSTR) __PHYSFS_smallAlloc(psize * sizeof (WCHAR));
        if (wstr != NULL)
        {
            if (pGetDir(accessToken, wstr, &psize))
                userDir = unicodeToUtf8Heap(wstr);
            __PHYSFS_smallFree(wstr);
        } /* if */

        CloseHandle(accessToken);
    } /* if */

    return 1;  /* We made it: hit the showers. */
} /* determineUserDir */


static BOOL mediaInDrive(const char *drive)
{
    UINT oldErrorMode;
    DWORD tmp;
    BOOL retval;

    /* Prevent windows warning message appearing when checking media size */
    /* !!! FIXME: Windows 7 offers SetThreadErrorMode(). */
    oldErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS);
    
    /* If this function succeeds, there's media in the drive */
    retval = GetVolumeInformationA(drive, NULL, 0, NULL, NULL, &tmp, NULL, 0);

    /* Revert back to old windows error handler */
    SetErrorMode(oldErrorMode);

    return retval;
} /* mediaInDrive */

/*
 * !!! FIXME: move this to a thread? This function hangs if you call it while
 * !!! FIXME:  a drive is spinning up right after inserting a disc.
 */
void __PHYSFS_platformDetectAvailableCDs(PHYSFS_StringCallback cb, void *data)
{
    /* !!! FIXME: Can CD drives be non-drive letter paths? */
    /* !!! FIXME:  (so can they be Unicode paths?) */
    char drive_str[4] = "x:\\";
    char ch;
    for (ch = 'A'; ch <= 'Z'; ch++)
    {
        drive_str[0] = ch;
        if (GetDriveTypeA(drive_str) == DRIVE_CDROM && mediaInDrive(drive_str))
            cb(data, drive_str);
    } /* for */
} /* __PHYSFS_platformDetectAvailableCDs */


char *__PHYSFS_platformCalcBaseDir(const char *argv0)
{
    DWORD buflen = 64;
    LPWSTR modpath = NULL;
    char *retval = NULL;

    while (1)
    {
        DWORD rc;
        void *ptr;

        if ( (ptr = allocator.Realloc(modpath, buflen*sizeof(WCHAR))) == NULL )
        {
            allocator.Free(modpath);
            BAIL_MACRO(ERR_OUT_OF_MEMORY, NULL);
        } /* if */
        modpath = (LPWSTR) ptr;

        rc = GetModuleFileNameW(NULL, modpath, buflen);
        if (rc == 0)
        {
            allocator.Free(modpath);
            BAIL_MACRO(winApiStrError(), NULL);
        } /* if */

        if (rc < buflen)
        {
            buflen = rc;
            break;
        } /* if */

        buflen *= 2;
    } /* while */

    if (buflen > 0)  /* just in case... */
    {
        WCHAR *ptr = (modpath + buflen) - 1;
        while (ptr != modpath)
        {
            if (*ptr == '\\')
                break;
            ptr--;
        } /* while */

        if ((ptr == modpath) && (*ptr != '\\'))
            __PHYSFS_setError(ERR_GETMODFN_NO_DIR);
        else
        {
            *(ptr + 1) = '\0';  /* chop off filename. */
            retval = unicodeToUtf8Heap(modpath);
        } /* else */
    } /* else */
    allocator.Free(modpath);

    return retval;   /* w00t. */
} /* __PHYSFS_platformCalcBaseDir */


char *__PHYSFS_platformGetUserName(void)
{
    DWORD bufsize = 0;
    char *retval = NULL;
    
    if (GetUserNameW(NULL, &bufsize) == 0)  /* This SHOULD fail. */
    {
        LPWSTR wbuf = (LPWSTR) __PHYSFS_smallAlloc(bufsize * sizeof (WCHAR));
        BAIL_IF_MACRO(wbuf == NULL, ERR_OUT_OF_MEMORY, NULL);
        if (GetUserNameW(wbuf, &bufsize) == 0)  /* ?! */
            __PHYSFS_setError(winApiStrError());
        else
            retval = unicodeToUtf8Heap(wbuf);
        __PHYSFS_smallFree(wbuf);
    } /* if */

    return retval;
} /* __PHYSFS_platformGetUserName */


char *__PHYSFS_platformGetUserDir(void)
{
    char *retval = (char *) allocator.Malloc(strlen(userDir) + 1);
    BAIL_IF_MACRO(retval == NULL, ERR_OUT_OF_MEMORY, NULL);
    strcpy(retval, userDir); /* calculated at init time. */
    return retval;
} /* __PHYSFS_platformGetUserDir */


void *__PHYSFS_platformGetThreadID(void)
{
    return ( (void *) ((size_t) GetCurrentThreadId()) );
} /* __PHYSFS_platformGetThreadID */


static int isSymlinkAttrs(const DWORD attr, const DWORD tag)
{
    return ( (attr & FILE_ATTRIBUTE_REPARSE_POINT) && 
             (tag == PHYSFS_IO_REPARSE_TAG_SYMLINK) );
} /* isSymlinkAttrs */


char *__PHYSFS_platformCvtToDependent(const char *prepend,
                                      const char *dirName,
                                      const char *append)
{
    const int len = ((prepend) ? strlen(prepend) : 0) +
              ((append) ? strlen(append) : 0) +
              strlen(dirName) + 1;
    char *retval = (char *) allocator.Malloc(len);
    char *p;

    BAIL_IF_MACRO(retval == NULL, ERR_OUT_OF_MEMORY, NULL);

    _snprintf(retval, len, "%s%s%s",
              prepend ? prepend : "", dirName, append ? append : "");

    for (p = strchr(retval, '/'); p != NULL; p = strchr(p + 1, '/'))
        *p = '\\';

    return retval;
} /* __PHYSFS_platformCvtToDependent */


void __PHYSFS_platformEnumerateFiles(const char *dirname,
                                     int omitSymLinks,
                                     PHYSFS_EnumFilesCallback callback,
                                     const char *origdir,
                                     void *callbackdata)
{
    HANDLE dir = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW entw;
    size_t len = strlen(dirname);
    char *searchPath = NULL;
    WCHAR *wSearchPath = NULL;

    /* Allocate a new string for path, maybe '\\', "*", and NULL terminator */
    searchPath = (char *) __PHYSFS_smallAlloc(len + 3);
    if (searchPath == NULL)
        return;

    /* Copy current dirname */
    strcpy(searchPath, dirname);

    /* if there's no '\\' at the end of the path, stick one in there. */
    if (searchPath[len - 1] != '\\')
    {
        searchPath[len++] = '\\';
        searchPath[len] = '\0';
    } /* if */

    /* Append the "*" to the end of the string */
    strcat(searchPath, "*");

    UTF8_TO_UNICODE_STACK_MACRO(wSearchPath, searchPath);
    if (wSearchPath == NULL)
        return;  /* oh well. */

    dir = FindFirstFileW(wSearchPath, &entw);

    __PHYSFS_smallFree(wSearchPath);
    __PHYSFS_smallFree(searchPath);
    if (dir == INVALID_HANDLE_VALUE)
        return;

    do
    {
        const DWORD attr = entw.dwFileAttributes;
        const DWORD tag = entw.dwReserved0;
        const WCHAR *fn = entw.cFileName;
        char *utf8;

        if ((fn[0] == '.') && (fn[1] == '\0'))
            continue;
        if ((fn[0] == '.') && (fn[1] == '.') && (fn[2] == '\0'))
            continue;
        if ((omitSymLinks) && (isSymlinkAttrs(attr, tag)))
            continue;

        utf8 = unicodeToUtf8Heap(fn);
        if (utf8 != NULL)
        {
            callback(callbackdata, origdir, utf8);
            allocator.Free(utf8);
        } /* if */
    } while (FindNextFileW(dir, &entw) != 0);

    FindClose(dir);
} /* __PHYSFS_platformEnumerateFiles */


char *__PHYSFS_platformCurrentDir(void)
{
    char *retval = NULL;
    WCHAR *wbuf = NULL;
    DWORD buflen = 0;

    buflen = GetCurrentDirectoryW(buflen, NULL);
    wbuf = (WCHAR *) __PHYSFS_smallAlloc((buflen + 2) * sizeof (WCHAR));
    BAIL_IF_MACRO(wbuf == NULL, ERR_OUT_OF_MEMORY, NULL);
    GetCurrentDirectoryW(buflen, wbuf);

    if (wbuf[buflen - 2] == '\\')
        wbuf[buflen - 1] = '\0';  /* just in case... */
    else
    {
        wbuf[buflen - 1] = '\\'; 
        wbuf[buflen] = '\0'; 
    } /* else */

    retval = unicodeToUtf8Heap(wbuf);
    __PHYSFS_smallFree(wbuf);
    return retval;
} /* __PHYSFS_platformCurrentDir */


/* this could probably use a cleanup. */
char *__PHYSFS_platformRealPath(const char *path)
{
    /*
     * At this point, we only use this for the user and base dir,
     *  and we already know those are RealPath'd by the OS for us.
     */
    char *retval = (char *) allocator.Malloc(strlen(path) + 1);
    BAIL_IF_MACRO(retval == NULL, ERR_OUT_OF_MEMORY, NULL);
    strcpy(retval, path);
    return retval;
} /* __PHYSFS_platformRealPath */


int __PHYSFS_platformMkDir(const char *path)
{
    WCHAR *wpath;
    DWORD rc;
    UTF8_TO_UNICODE_STACK_MACRO(wpath, path);
    rc = CreateDirectoryW(wpath, NULL);
    __PHYSFS_smallFree(wpath);
    BAIL_IF_MACRO(rc == 0, winApiStrError(), 0);
    return 1;
} /* __PHYSFS_platformMkDir */


int __PHYSFS_platformInit(void)
{
    libUserEnv = LoadLibraryA("userenv.dll");
    BAIL_IF_MACRO(libUserEnv == NULL, winApiStrError(), 0);

    /* !!! FIXME: why do we precalculate this? */
    BAIL_IF_MACRO(!determineUserDir(), NULL, 0);
    return 1;  /* It's all good */
} /* __PHYSFS_platformInit */


int __PHYSFS_platformDeinit(void)
{
    if (libUserEnv)
        FreeLibrary(libUserEnv);
    libUserEnv = NULL;
    allocator.Free(userDir);
    userDir = NULL;
    return 1; /* It's all good */
} /* __PHYSFS_platformDeinit */


static void *doOpen(const char *fname, DWORD mode, DWORD creation, int rdonly)
{
    HANDLE fileh;
    WinApiFile *retval;
    WCHAR *wfname;

    UTF8_TO_UNICODE_STACK_MACRO(wfname, fname);
    BAIL_IF_MACRO(wfname == NULL, ERR_OUT_OF_MEMORY, NULL);
    fileh = CreateFileW(wfname, mode, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, creation, FILE_ATTRIBUTE_NORMAL, NULL);
    __PHYSFS_smallFree(wfname);

    BAIL_IF_MACRO(fileh == INVALID_HANDLE_VALUE,winApiStrError(), NULL);

    retval = (WinApiFile *) allocator.Malloc(sizeof (WinApiFile));
    if (retval == NULL)
    {
        CloseHandle(fileh);
        BAIL_MACRO(ERR_OUT_OF_MEMORY, NULL);
    } /* if */

    retval->readonly = rdonly;
    retval->handle = fileh;
    return retval;
} /* doOpen */


void *__PHYSFS_platformOpenRead(const char *filename)
{
    return doOpen(filename, GENERIC_READ, OPEN_EXISTING, 1);
} /* __PHYSFS_platformOpenRead */


void *__PHYSFS_platformOpenWrite(const char *filename)
{
    return doOpen(filename, GENERIC_WRITE, CREATE_ALWAYS, 0);
} /* __PHYSFS_platformOpenWrite */


void *__PHYSFS_platformOpenAppend(const char *filename)
{
    void *retval = doOpen(filename, GENERIC_WRITE, OPEN_ALWAYS, 0);
    if (retval != NULL)
    {
        HANDLE h = ((WinApiFile *) retval)->handle;
        DWORD rc = SetFilePointer(h, 0, NULL, FILE_END);
        if (rc == PHYSFS_INVALID_SET_FILE_POINTER)
        {
            const char *err = winApiStrError();
            CloseHandle(h);
            allocator.Free(retval);
            BAIL_MACRO(err, NULL);
        } /* if */
    } /* if */

    return retval;
} /* __PHYSFS_platformOpenAppend */


/* !!! FIXME: this function fails if len > 0xFFFFFFFF. */
PHYSFS_sint64 __PHYSFS_platformRead(void *opaque, void *buf, PHYSFS_uint64 len)
{
    HANDLE Handle = ((WinApiFile *) opaque)->handle;
    DWORD CountOfBytesRead = 0;

    BAIL_IF_MACRO(!__PHYSFS_ui64FitsAddressSpace(len),ERR_INVALID_ARGUMENT,-1);

    if(!ReadFile(Handle, buf, (DWORD) len, &CountOfBytesRead, NULL))
        BAIL_MACRO(winApiStrError(), -1);
    return (PHYSFS_sint64) CountOfBytesRead;
} /* __PHYSFS_platformRead */


/* !!! FIXME: this function fails if len > 0xFFFFFFFF. */
PHYSFS_sint64 __PHYSFS_platformWrite(void *opaque, const void *buffer,
                                     PHYSFS_uint64 len)
{
    HANDLE Handle = ((WinApiFile *) opaque)->handle;
    DWORD CountOfBytesWritten = 0;

    BAIL_IF_MACRO(!__PHYSFS_ui64FitsAddressSpace(len),ERR_INVALID_ARGUMENT,-1);

    if(!WriteFile(Handle, buffer, (DWORD) len, &CountOfBytesWritten, NULL))
        BAIL_MACRO(winApiStrError(), -1);
    return (PHYSFS_sint64) CountOfBytesWritten;
} /* __PHYSFS_platformWrite */


int __PHYSFS_platformSeek(void *opaque, PHYSFS_uint64 pos)
{
    HANDLE Handle = ((WinApiFile *) opaque)->handle;
    LONG HighOrderPos;
    PLONG pHighOrderPos;
    DWORD rc;

    /* Get the high order 32-bits of the position */
    HighOrderPos = HIGHORDER_UINT64(pos);

    /*
     * MSDN: "If you do not need the high-order 32 bits, this
     *         pointer must be set to NULL."
     */
    pHighOrderPos = (HighOrderPos) ? &HighOrderPos : NULL;

    /*
     * !!! FIXME: MSDN: "Windows Me/98/95:  If the pointer
     * !!! FIXME:  lpDistanceToMoveHigh is not NULL, then it must
     * !!! FIXME:  point to either 0, INVALID_SET_FILE_POINTER, or
     * !!! FIXME:  the sign extension of the value of lDistanceToMove.
     * !!! FIXME:  Any other value will be rejected."
     */

    /* Move pointer "pos" count from start of file */
    rc = SetFilePointer(Handle, LOWORDER_UINT64(pos),
                        pHighOrderPos, FILE_BEGIN);

    if ( (rc == PHYSFS_INVALID_SET_FILE_POINTER) &&
         (GetLastError() != NO_ERROR) )
    {
        BAIL_MACRO(winApiStrError(), 0);
    } /* if */
    
    return 1;  /* No error occured */
} /* __PHYSFS_platformSeek */


PHYSFS_sint64 __PHYSFS_platformTell(void *opaque)
{
    HANDLE Handle = ((WinApiFile *) opaque)->handle;
    LONG HighPos = 0;
    DWORD LowPos;
    PHYSFS_sint64 retval;

    /* Get current position */
    LowPos = SetFilePointer(Handle, 0, &HighPos, FILE_CURRENT);
    if ( (LowPos == PHYSFS_INVALID_SET_FILE_POINTER) &&
         (GetLastError() != NO_ERROR) )
    {
        BAIL_MACRO(winApiStrError(), -1);
    } /* if */
    else
    {
        /* Combine the high/low order to create the 64-bit position value */
        retval = (((PHYSFS_uint64) HighPos) << 32) | LowPos;
        assert(retval >= 0);
    } /* else */

    return retval;
} /* __PHYSFS_platformTell */


PHYSFS_sint64 __PHYSFS_platformFileLength(void *opaque)
{
    HANDLE Handle = ((WinApiFile *) opaque)->handle;
    DWORD SizeHigh;
    DWORD SizeLow;
    PHYSFS_sint64 retval;

    SizeLow = GetFileSize(Handle, &SizeHigh);
    if ( (SizeLow == PHYSFS_INVALID_SET_FILE_POINTER) &&
         (GetLastError() != NO_ERROR) )
    {
        BAIL_MACRO(winApiStrError(), -1);
    } /* if */
    else
    {
        /* Combine the high/low order to create the 64-bit position value */
        retval = (((PHYSFS_uint64) SizeHigh) << 32) | SizeLow;
        assert(retval >= 0);
    } /* else */

    return retval;
} /* __PHYSFS_platformFileLength */


int __PHYSFS_platformFlush(void *opaque)
{
    WinApiFile *fh = ((WinApiFile *) opaque);
    if (!fh->readonly)
        BAIL_IF_MACRO(!FlushFileBuffers(fh->handle), winApiStrError(), 0);

    return 1;
} /* __PHYSFS_platformFlush */


void __PHYSFS_platformClose(void *opaque)
{
    HANDLE Handle = ((WinApiFile *) opaque)->handle;
    (void) CloseHandle(Handle); /* ignore errors. You should have flushed! */
    allocator.Free(opaque);
} /* __PHYSFS_platformClose */


static int doPlatformDelete(LPWSTR wpath)
{
    const int isdir = (GetFileAttributesW(wpath) & FILE_ATTRIBUTE_DIRECTORY);
    const BOOL rc = (isdir) ? RemoveDirectoryW(wpath) : DeleteFileW(wpath);
    BAIL_IF_MACRO(!rc, winApiStrError(), 0);
    return 1;   /* if you made it here, it worked. */
} /* doPlatformDelete */


int __PHYSFS_platformDelete(const char *path)
{
    int retval = 0;
    LPWSTR wpath = NULL;
    UTF8_TO_UNICODE_STACK_MACRO(wpath, path);
    BAIL_IF_MACRO(wpath == NULL, ERR_OUT_OF_MEMORY, 0);
    retval = doPlatformDelete(wpath);
    __PHYSFS_smallFree(wpath);
    return retval;
} /* __PHYSFS_platformDelete */


/*
 * !!! FIXME: why aren't we using Critical Sections instead of Mutexes?
 * !!! FIXME:  mutexes on Windows are for cross-process sync. CritSects are
 * !!! FIXME:  mutexes for threads in a single process and are faster.
 */
void *__PHYSFS_platformCreateMutex(void)
{
    return ((void *) CreateMutex(NULL, FALSE, NULL));
} /* __PHYSFS_platformCreateMutex */


void __PHYSFS_platformDestroyMutex(void *mutex)
{
    CloseHandle((HANDLE) mutex);
} /* __PHYSFS_platformDestroyMutex */


int __PHYSFS_platformGrabMutex(void *mutex)
{
    return (WaitForSingleObject((HANDLE) mutex, INFINITE) != WAIT_FAILED);
} /* __PHYSFS_platformGrabMutex */


void __PHYSFS_platformReleaseMutex(void *mutex)
{
    ReleaseMutex((HANDLE) mutex);
} /* __PHYSFS_platformReleaseMutex */


static PHYSFS_sint64 FileTimeToPhysfsTime(const FILETIME *ft)
{
    SYSTEMTIME st_utc;
    SYSTEMTIME st_localtz;
    TIME_ZONE_INFORMATION tzi;
    DWORD tzid;
    PHYSFS_sint64 retval;
    struct tm tm;
    BOOL rc;

    BAIL_IF_MACRO(!FileTimeToSystemTime(ft, &st_utc), winApiStrError(), -1);
    tzid = GetTimeZoneInformation(&tzi);
    BAIL_IF_MACRO(tzid == TIME_ZONE_ID_INVALID, winApiStrError(), -1);
    rc = SystemTimeToTzSpecificLocalTime(&tzi, &st_utc, &st_localtz);
    BAIL_IF_MACRO(!rc, winApiStrError(), -1);

    /* Convert to a format that mktime() can grok... */
    tm.tm_sec = st_localtz.wSecond;
    tm.tm_min = st_localtz.wMinute;
    tm.tm_hour = st_localtz.wHour;
    tm.tm_mday = st_localtz.wDay;
    tm.tm_mon = st_localtz.wMonth - 1;
    tm.tm_year = st_localtz.wYear - 1900;
    tm.tm_wday = -1 /*st_localtz.wDayOfWeek*/;
    tm.tm_yday = -1;
    tm.tm_isdst = -1;

    /* Convert to a format PhysicsFS can grok... */
    retval = (PHYSFS_sint64) mktime(&tm);
    BAIL_IF_MACRO(retval == -1, strerror(errno), -1);
    return retval;
} /* FileTimeToPhysfsTime */

int __PHYSFS_platformStat(const char *filename, int *exists, PHYSFS_Stat *stat)
{
    WIN32_FILE_ATTRIBUTE_DATA winstat;
    WCHAR *wstr = NULL;
    DWORD err = 0;
    BOOL rc = 0;

    UTF8_TO_UNICODE_STACK_MACRO(wstr, filename);
    BAIL_IF_MACRO(wstr == NULL, ERR_OUT_OF_MEMORY, 0);
    rc = GetFileAttributesExW(wstr, GetFileExInfoStandard, &winstat);
    err = (!rc) ? GetLastError() : 0;
    *exists = ((err != ERROR_FILE_NOT_FOUND) && (err != ERROR_PATH_NOT_FOUND));
    __PHYSFS_smallFree(wstr);
    BAIL_IF_MACRO(!rc, winApiStrErrorByNum(err), 0);

    stat->modtime = FileTimeToPhysfsTime(&winstat.ftLastWriteTime);
    stat->accesstime = FileTimeToPhysfsTime(&winstat.ftLastAccessTime);
    stat->createtime = FileTimeToPhysfsTime(&winstat.ftCreationTime);

    if(winstat.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
        stat->filetype = PHYSFS_FILETYPE_DIRECTORY;
        stat->filesize = 0;
    } /* if */

    else if(winstat.dwFileAttributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_DEVICE))
    {
        /* !!! FIXME: what are reparse points? */
        stat->filetype = PHYSFS_FILETYPE_OTHER;
        /* !!! FIXME: don't rely on this */
        stat->filesize = 0;
    } /* else if */

    /* !!! FIXME: check for symlinks on Vista. */

    else
    {
        stat->filetype = PHYSFS_FILETYPE_REGULAR;
        stat->filesize = (((PHYSFS_uint64) winstat.nFileSizeHigh) << 32) | winstat.nFileSizeLow;
    } /* else */

    stat->readonly = ((winstat.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0);

    return 1;
} /* __PHYSFS_platformStat */


/* !!! FIXME: Don't use C runtime for allocators? */
int __PHYSFS_platformSetDefaultAllocator(PHYSFS_Allocator *a)
{
    return 0;  /* just use malloc() and friends. */
} /* __PHYSFS_platformSetDefaultAllocator */

#endif  /* PHYSFS_PLATFORM_WINDOWS */

/* end of windows.c ... */

