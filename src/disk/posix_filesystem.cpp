// Prospero Disk - The real file system through opendir, lstat and statvfs.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "disk/filesystem.hpp"

#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#if defined(__PROSPERO__)
#include <cstring>
#include <fcntl.h>
#include <sys/mount.h>
#else
#include <sys/statvfs.h>
#endif
#include <unistd.h>

#if defined(__PROSPERO__)
extern "C" int sceKernelDlsym(int module, const char *name, void **address);
#endif

namespace hui::disk
{

namespace
{

#if defined(__PROSPERO__)
// statfs, fstatfs and statvfs are libkernel_sys exports, which an app process
// is not given: calling them jumps through a null import and ends the process
// (CE-108255-1). The app's own libkernel exports the same call as _fstatfs;
// resolve it by name instead (docs/platform/CONSOLE_NOTES.md).
using Fstatfs = int (*)(int, struct statfs *);

Fstatfs resolved_fstatfs()
{
    static const Fstatfs query = []
    {
        void *symbol = nullptr;
        if (sceKernelDlsym(0x2001, "_fstatfs", &symbol) != 0)
            return Fstatfs{};
        return reinterpret_cast<Fstatfs>(symbol);
    }();
    return query;
}
#endif

EntryType type_of(mode_t mode)
{
    if (S_ISDIR(mode))
        return EntryType::directory;
    if (S_ISLNK(mode))
        return EntryType::symlink;
    if (S_ISREG(mode))
        return EntryType::file;
    return EntryType::other;
}

void fill(const struct stat &st, Entry *out)
{
    out->type = type_of(st.st_mode);
    out->device = static_cast<std::uint64_t>(st.st_dev);
    out->inode = static_cast<std::uint64_t>(st.st_ino);
    out->links = static_cast<std::uint32_t>(st.st_nlink);
    out->disk = static_cast<std::uint64_t>(st.st_blocks) * 512u;
    out->apparent = st.st_size > 0 ? static_cast<std::uint64_t>(st.st_size) : 0u;
    out->mtime = static_cast<std::int64_t>(st.st_mtime);
}

class PosixFileSystem final : public FileSystem
{
  public:
    PosixFileSystem(bool allow_space, bool allow_remove)
        : allow_space_(allow_space), allow_remove_(allow_remove)
    {
    }

    bool list(const std::string &path, std::vector<Entry> *out, int *error) const override
    {
        out->clear();
        DIR *dir = opendir(path.c_str());
        if (dir == nullptr)
        {
            *error = errno;
            return false;
        }
        // lstat by full path: the console's libc has neither dirfd nor fstatat.
        std::string full = path;
        if (full.empty() || full.back() != '/')
            full.push_back('/');
        const std::size_t base = full.size();
        for (dirent *d = readdir(dir); d != nullptr; d = readdir(dir))
        {
            const char *name = d->d_name;
            if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
                continue;
            full.resize(base);
            full.append(name);
            struct stat st
            {
            };
            if (lstat(full.c_str(), &st) != 0)
                continue; // removed while we read: it no longer uses space
            Entry entry;
            entry.name = name;
            fill(st, &entry);
            out->push_back(std::move(entry));
        }
        closedir(dir);
        return true;
    }

    bool stat(const std::string &path, Entry *out) const override
    {
        struct stat st
        {
        };
        if (lstat(path.c_str(), &st) != 0)
            return false; // errno from lstat
        const std::size_t slash = path.find_last_of('/');
        out->name = slash == std::string::npos ? path : path.substr(slash + 1);
        fill(st, out);
        return true;
    }

    // unlink and rmdir are libkernel exports, which an app may call; unlinkat
    // is only in libkernel_sys and would end the process (CONSOLE_NOTES 3).
    bool remove_file(const std::string &path, int *error) const override
    {
        if (!allow_remove_)
        {
            *error = EROFS;
            return false;
        }
        if (unlink(path.c_str()) == 0)
            return true;
        *error = errno;
        return false;
    }

    bool remove_folder(const std::string &path, int *error) const override
    {
        if (!allow_remove_)
        {
            *error = EROFS;
            return false;
        }
        if (rmdir(path.c_str()) == 0)
            return true;
        *error = errno;
        return false;
    }

    bool move(const std::string &from, const std::string &to, int *error) const override
    {
        if (!allow_remove_)
        {
            *error = EROFS;
            return false;
        }
        struct stat st
        {
        };
        if (lstat(to.c_str(), &st) == 0)
        {
            *error = EEXIST;
            return false;
        }
        if (rename(from.c_str(), to.c_str()) == 0)
            return true;
        *error = errno;
        return false;
    }

    bool space(const std::string &path, SpaceInfo *out) const override
    {
        if (!allow_space_)
            return false;
#if defined(__PROSPERO__)
        const Fstatfs query = resolved_fstatfs();
        if (query == nullptr)
            return false;
        const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY);
        if (fd < 0)
            return false;
        struct statfs info;
        std::memset(&info, 0, sizeof info);
        const bool ok = query(fd, &info) == 0;
        close(fd);
        if (!ok || info.f_bsize == 0)
            return false;
        const std::uint64_t unit = info.f_bsize;
        out->total = static_cast<std::uint64_t>(info.f_blocks) * unit;
        out->free = static_cast<std::uint64_t>(info.f_bavail < 0 ? 0 : info.f_bavail) * unit;
        const std::uint64_t unused = static_cast<std::uint64_t>(info.f_bfree) * unit;
        out->used = out->total > unused ? out->total - unused : 0u;
        return true;
#else
        struct statvfs vfs
        {
        };
        if (statvfs(path.c_str(), &vfs) != 0)
            return false;
        const std::uint64_t unit = vfs.f_frsize != 0 ? vfs.f_frsize : vfs.f_bsize;
        out->total = static_cast<std::uint64_t>(vfs.f_blocks) * unit;
        out->free = static_cast<std::uint64_t>(vfs.f_bavail) * unit;
        const std::uint64_t unused = static_cast<std::uint64_t>(vfs.f_bfree) * unit;
        out->used = out->total > unused ? out->total - unused : 0u;
        return true;
#endif
    }

    bool read_file(const std::string &path, std::string *out, std::size_t max_bytes) const override
    {
        std::FILE *file = std::fopen(path.c_str(), "rb");
        if (file == nullptr)
            return false;
        out->clear();
        char buffer[16384];
        bool ok = true;
        for (;;)
        {
            const std::size_t got = std::fread(buffer, 1, sizeof buffer, file);
            out->append(buffer, got);
            if (out->size() > max_bytes)
            {
                ok = false;
                break;
            }
            if (got < sizeof buffer)
            {
                ok = std::ferror(file) == 0;
                break;
            }
        }
        std::fclose(file);
        return ok;
    }

  private:
    bool allow_space_;
    bool allow_remove_;
};

} // namespace

std::unique_ptr<FileSystem> make_posix_filesystem(bool allow_space, bool allow_remove)
{
    return std::make_unique<PosixFileSystem>(allow_space, allow_remove);
}

} // namespace hui::disk
