// Prospero Disk - The M1 storage probe: what the elevation made readable (docs/PRD.md 10.1-3).
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/storage_probe.hpp"

#include "disk/volumes.hpp"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <dirent.h>
#include <functional>
#include <string>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <vector>

namespace hui::ps5
{

namespace
{

const std::function<void(const std::string &)> *g_emit = nullptr;

void line(std::string &out, const char *format, ...) __attribute__((format(printf, 2, 3)));

void line(std::string &out, const char *format, ...)
{
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof text, format, args);
    va_end(args);
    out += text;
    out += '\n';
    if (g_emit != nullptr)
        (*g_emit)(text);
}

unsigned long long gib(unsigned long long bytes)
{
    return bytes >> 30;
}

void probe_path(std::string &out, const std::string &path)
{
    struct stat st
    {
    };
    // One line before each call: the sandbox ends the process on some of them.
    line(out, "  lstat %s", path.c_str());
    const bool stat_ok = lstat(path.c_str(), &st) == 0;
    const int stat_error = stat_ok ? 0 : errno;
    int entries = -1;
    int list_error = 0;
    line(out, "  opendir %s", path.c_str());
    if (DIR *dir = opendir(path.c_str()))
    {
        entries = 0;
        line(out, "  readdir %s", path.c_str());
        while (readdir(dir) != nullptr)
            ++entries;
        closedir(dir);
    }
    else
        list_error = errno;
    struct statvfs vfs
    {
    };
    line(out, "  statvfs %s", path.c_str());
    const bool vfs_ok = statvfs(path.c_str(), &vfs) == 0;
    const unsigned long long unit = vfs.f_frsize != 0 ? vfs.f_frsize : vfs.f_bsize;
    line(out,
         "path %s stat=%s(errno %d) dev=%llu list=%d(errno %d) statvfs=%s total=%lluGiB "
         "used=%lluGiB",
         path.c_str(), stat_ok ? "ok" : "fail", stat_error,
         static_cast<unsigned long long>(st.st_dev), entries, list_error, vfs_ok ? "ok" : "fail",
         gib(static_cast<unsigned long long>(vfs.f_blocks) * unit),
         gib(static_cast<unsigned long long>(vfs.f_blocks - vfs.f_bfree) * unit));
}

// st_blocks x 512 against st_size for the first regular files under a folder:
// is the block count meaningful on this file system (PRD 10.3)?
void probe_blocks(std::string &out, const std::string &root, int depth, int *budget)
{
    DIR *dir = opendir(root.c_str());
    if (dir == nullptr)
        return;
    for (dirent *d = readdir(dir); d != nullptr && *budget > 0; d = readdir(dir))
    {
        const std::string name = d->d_name;
        if (name == "." || name == "..")
            continue;
        const std::string path = root + "/" + name;
        struct stat st
        {
        };
        if (lstat(path.c_str(), &st) != 0)
            continue;
        if (S_ISREG(st.st_mode) && st.st_size > 0)
        {
            line(out, "blocks %s size=%lld blocks*512=%lld", path.c_str(),
                 static_cast<long long>(st.st_size), static_cast<long long>(st.st_blocks) * 512);
            --*budget;
        }
        else if (S_ISDIR(st.st_mode) && depth > 0)
            probe_blocks(out, path, depth - 1, budget);
    }
    closedir(dir);
}

} // namespace

std::string probe_storage(const std::function<void(const std::string &)> &emit)
{
    g_emit = &emit;
    std::string out;
    line(out, "prospero-disk storage probe");

    std::vector<std::string> paths = {"/",
                                      "/system",
                                      "/system_data",
                                      "/preinst/common/font",
                                      "/user/appmeta",
                                      "/system_data/priv/mms"};
    for (const disk::VolumeSpec &spec : disk::console_volume_specs())
        paths.push_back(spec.path);
    for (const std::string &path : paths)
        probe_path(out, path);
    probe_path(out, "/system_data/priv/mms/app.db");

    int budget = 6;
    probe_blocks(out, "/user/app", 2, &budget);
    budget = 3;
    probe_blocks(out, "/mnt/usb0", 2, &budget);

    line(out, "getfsstat next");
    const int count = getfsstat(nullptr, 0, MNT_NOWAIT);
    line(out, "mounts %d (errno %d)", count, count < 0 ? errno : 0);
    if (count > 0)
    {
        std::vector<struct statfs> mounts(static_cast<std::size_t>(count) + 8);
        const int got = getfsstat(
            mounts.data(), static_cast<long>(mounts.size() * sizeof(struct statfs)), MNT_NOWAIT);
        for (int i = 0; i < got; ++i)
        {
            const struct statfs &m = mounts[static_cast<std::size_t>(i)];
            line(out, "mount %s on %s type %s flags=0x%llx total=%lluGiB", m.f_mntfromname,
                 m.f_mntonname, m.f_fstypename, static_cast<unsigned long long>(m.f_flags),
                 gib(static_cast<unsigned long long>(m.f_blocks) * m.f_bsize));
        }
    }
    g_emit = nullptr;
    return out;
}

} // namespace hui::ps5
