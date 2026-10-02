// tests/posix_compat.h -- the POSIX calls the tests use, on Windows too.
//
// The library goes through decnet/common/platform.h.  The tests also
// poke at the environment and the file system directly; on Windows this
// supplies those calls, and on POSIX it is just the system headers.

#ifndef DNTEST_POSIX_COMPAT_H
#define DNTEST_POSIX_COMPAT_H

#include "decnet/common/platform.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#ifdef _WIN32

#include <direct.h>
#include <io.h>
#include <process.h>
#include <windows.h>

inline int setenv (const char *name, const char *value, int overwrite)
{
    if (!overwrite && std::getenv (name)) return 0;
    return ::_putenv_s (name, value);
}

// Setting a variable to "" removes it on Windows.
inline int unsetenv (const char *name) { return ::_putenv_s (name, ""); }

#ifndef F_OK
#define F_OK 0
#endif

inline int mkdir (const char *path, int) { return ::_mkdir (path); }

inline char *mkdtemp (char *tmpl)
{
    std::size_t len = std::strlen (tmpl) + 1;
    std::string base (tmpl);
    for (int tries = 0; tries < 100; ++tries) {
        std::memcpy (tmpl, base.c_str (), len);
        if (::_mktemp_s (tmpl, len) != 0) return nullptr;
        if (::_mkdir (tmpl) == 0) return tmpl;
    }
    return nullptr;
}

// Needs Developer Mode, or an elevated process; fails otherwise.
inline int symlink (const char *target, const char *link)
{
    DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    if (std::filesystem::is_directory (target))
        flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
    return ::CreateSymbolicLinkA (link, target, flags) ? 0 : -1;
}

#else

#include <sys/stat.h>
#include <unistd.h>

#endif

namespace dntest {

// Where a test may put scratch files: /tmp, or %TEMP% on Windows, with
// '/' separators either way.
inline std::string tmp_dir ()
{
#ifdef _WIN32
    std::error_code ec;
    auto d = std::filesystem::temp_directory_path (ec);
    if (!ec) {
        std::string s = d.generic_string ();
        if (!s.empty () && s.back () == '/') s.pop_back ();
        return s;
    }
#endif
    return "/tmp";
}

}   // namespace dntest

#endif  // DNTEST_POSIX_COMPAT_H
