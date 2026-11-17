// Prospero Disk - The real file system through opendir, fstatat and statvfs.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "disk/filesystem.hpp"

#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace hui::disk
{

namespace
{

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
    bool list(const std::string &path, std::vector<Entry> *out, int *error) const override
    {
        out->clear();
        DIR *dir = opendir(path.c_str());
        if (dir == nullptr)
        {
            *error = errno;
            return false;
        }
        const int fd = dirfd(dir);
        // readdir on its own DIR is safe across threads; each thread opens its own.
        for (dirent *d = readdir(dir); d != nullptr; d = readdir(dir))
        {
            const char *name = d->d_name;
            if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
                continue;
            struct stat st
            {
            };
            if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
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
            return false;
        const std::size_t slash = path.find_last_of('/');
        out->name = slash == std::string::npos ? path : path.substr(slash + 1);
        fill(st, out);
        return true;
    }

    bool space(const std::string &path, SpaceInfo *out) const override
    {
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
};

} // namespace

std::unique_ptr<FileSystem> make_posix_filesystem()
{
    return std::make_unique<PosixFileSystem>();
}

} // namespace hui::disk
