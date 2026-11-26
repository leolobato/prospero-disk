// Prospero Disk - The write probe: what the elevated app may create and remove, and where.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/write_probe.hpp"

#include "platform/ps5/system.hpp"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace hui::ps5
{

namespace
{

class Probe
{
  public:
    Probe(uid_t user, const std::function<void(const std::string &)> &emit)
        : user_(user), emit_(emit)
    {
    }

    void line(const char *format, ...) __attribute__((format(printf, 2, 3)))
    {
        char text[512];
        va_list args;
        va_start(args, format);
        std::vsnprintf(text, sizeof text, format, args);
        va_end(args);
        out_ += text;
        out_ += '\n';
        emit_(text);
    }

    uid_t user() const
    {
        return user_;
    }

    // Runs call with the effective uid set to as and returns its errno (0 for
    // success). The line before names the call: a call that ends the process
    // still leaves it in the log.
    template <typename Call>
    int step(const char *what, const std::string &path, uid_t as, Call call)
    {
        line("  %s %s as euid %d", what, path.c_str(), static_cast<int>(as));
        int error = 0;
        if (geteuid() != as && seteuid(as) != 0)
            error = errno;
        else if (!call())
            error = errno != 0 ? errno : EIO;
        restore();
        line("%s %s as=%d -> %s", what, path.c_str(), static_cast<int>(as),
             error == 0 ? "ok" : std::strerror(error));
        return error;
    }

    void owner(const std::string &path)
    {
        struct stat st
        {
        };
        if (lstat(path.c_str(), &st) != 0)
        {
            line("owner %s: %s", path.c_str(), std::strerror(errno));
            return;
        }
        const char *type = S_ISDIR(st.st_mode)   ? "dir"
                           : S_ISLNK(st.st_mode) ? "link"
                           : S_ISREG(st.st_mode) ? "file"
                                                 : "other";
        line("owner %s %s uid=%d gid=%d mode=%04o dev=%llu", path.c_str(), type,
             static_cast<int>(st.st_uid), static_cast<int>(st.st_gid),
             static_cast<unsigned>(st.st_mode & 07777), static_cast<unsigned long long>(st.st_dev));
    }

    std::string take()
    {
        return std::move(out_);
    }

  private:
    void restore()
    {
        if (geteuid() == user_ || seteuid(user_) == 0)
            return;
        // Everything after this would run as root and leave files the app
        // cannot change later: stop here, before any other thread starts.
        line("fatal: effective uid restore failed: %s", std::strerror(errno));
        sys::park();
    }

    uid_t user_;
    const std::function<void(const std::string &)> &emit_;
    std::string out_;
};

// A folder the app's user may not search (root's 0700 /user/download) hides
// what is in it: count that as present, so the clean-up still tries to remove
// it with the euid raised instead of leaving it behind.
bool exists(const std::string &path)
{
    struct stat st
    {
    };
    return lstat(path.c_str(), &st) == 0 || errno == EACCES;
}

bool make_dir(const std::string &path, mode_t mode)
{
    // chmod after mkdir: the umask would otherwise trim the mode.
    return mkdir(path.c_str(), 0777) == 0 && chmod(path.c_str(), mode) == 0;
}

bool make_file(const std::string &path)
{
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0)
        return false;
    const bool ok = write(fd, "pd\n", 3) == 3;
    return close(fd) == 0 && ok;
}

bool remove_file(const std::string &path)
{
    return unlink(path.c_str()) == 0;
}

bool remove_dir(const std::string &path)
{
    return rmdir(path.c_str()) == 0;
}

// Removes what an earlier, interrupted probe left: a file and then its folder,
// trying the app's user first, then root.
void clear(Probe &p, const std::string &dir, const char *file)
{
    if (!exists(dir))
        return;
    p.line("leftover %s", dir.c_str());
    const std::string inner = dir + "/" + file;
    for (const uid_t as : {p.user(), static_cast<uid_t>(0)})
        if (exists(inner))
            p.step("unlink", inner, as, [&] { return remove_file(inner); });
    for (const uid_t as : {p.user(), static_cast<uid_t>(0)})
        if (exists(dir))
            p.step("rmdir", dir, as, [&] { return remove_dir(dir); });
}

// One discriminating case in the scratch folder: a folder of a given mode made
// by one user, a file in it made by another, then removed by each in turn.
void folder_case(Probe &p, const std::string &base, const char *name, uid_t folder_owner,
                 mode_t mode, uid_t file_owner)
{
    const std::string dir = base + "/" + name;
    const std::string file = dir + "/f";
    p.line("case %s: folder %04o by euid %d, file by euid %d", name, static_cast<unsigned>(mode),
           static_cast<int>(folder_owner), static_cast<int>(file_owner));
    clear(p, dir, "f");
    if (p.step("mkdir", dir, folder_owner, [&] { return make_dir(dir, 0777); }) != 0)
        return;
    // The file is made while the folder is still open to everyone, then the
    // folder gets the mode under test.
    p.step("create", file, file_owner, [&] { return make_file(file); });
    p.step("chmod", dir, folder_owner, [&] { return chmod(dir.c_str(), mode) == 0; });
    p.owner(dir);
    p.owner(file);
    const uid_t other = folder_owner == 0 ? p.user() : 0;
    for (const uid_t as : {other, folder_owner})
        if (exists(file))
            p.step("unlink", file, as, [&] { return remove_file(file); });
    p.step("chmod", dir, folder_owner, [&] { return chmod(dir.c_str(), 0777) == 0; });
    clear(p, dir, "f");
}

// Creates and removes a throwaway folder and file inside target: as the app's
// user, or with the euid raised when the user cannot create there.
void location(Probe &p, const std::string &target, const std::string &stamp)
{
    p.owner(target);
    if (!exists(target))
        return;
    const std::string dir = target + "/.pd-probe-" + stamp;
    const std::string file = dir + "/probe.bin";
    clear(p, dir, "probe.bin");
    uid_t maker = p.user();
    if (p.step("mkdir", dir, maker, [&] { return make_dir(dir, 0755); }) != 0)
    {
        maker = 0;
        if (p.step("mkdir", dir, maker, [&] { return make_dir(dir, 0755); }) != 0)
        {
            p.line("result %s create=none", target.c_str());
            return;
        }
    }
    p.step("create", file, maker, [&] { return make_file(file); });
    p.owner(dir);
    int unlink_by = -1;
    for (const uid_t as : {p.user(), static_cast<uid_t>(0)})
        if (exists(file) && p.step("unlink", file, as, [&] { return remove_file(file); }) == 0)
            unlink_by = static_cast<int>(as);
    int rmdir_by = -1;
    for (const uid_t as : {p.user(), static_cast<uid_t>(0)})
        if (exists(dir) && p.step("rmdir", dir, as, [&] { return remove_dir(dir); }) == 0)
            rmdir_by = static_cast<int>(as);
    p.line("result %s create=%d unlink=%d rmdir=%d%s", target.c_str(), static_cast<int>(maker),
           unlink_by, rmdir_by, exists(dir) ? " LEFT BEHIND" : "");
}

// Owner and mode of the first few entries of a folder; changes nothing.
void list_owners(Probe &p, const std::string &dir, int count)
{
    p.line("  opendir %s", dir.c_str());
    DIR *d = opendir(dir.c_str());
    if (d == nullptr)
    {
        p.line("list %s: %s", dir.c_str(), std::strerror(errno));
        return;
    }
    std::vector<std::string> names;
    for (dirent *e = readdir(d); e != nullptr && static_cast<int>(names.size()) < count;
         e = readdir(d))
        if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0)
            names.emplace_back(e->d_name);
    closedir(d);
    p.owner(dir);
    for (const std::string &name : names)
        p.owner(dir + "/" + name);
}

// The first entry of a folder that is not hidden ("" when none).
std::string first_entry(const std::string &dir)
{
    DIR *d = opendir(dir.c_str());
    if (d == nullptr)
        return {};
    std::string found;
    for (dirent *e = readdir(d); e != nullptr && found.empty(); e = readdir(d))
        if (e->d_name[0] != '.')
            found = e->d_name;
    closedir(d);
    return found;
}

// A drive's folder under /mnt is a mounted drive only when it is on another
// device than /mnt (src/disk/volumes.cpp, is_mounted).
bool mounted(const std::string &path)
{
    struct stat self
    {
    };
    struct stat parent
    {
    };
    return lstat(path.c_str(), &self) == 0 && lstat("/mnt", &parent) == 0 &&
           self.st_dev != parent.st_dev;
}

} // namespace

std::string probe_writes(const std::string &data_root, uid_t app_user,
                         const std::function<void(const std::string &)> &emit)
{
    Probe p(app_user, emit);
    p.line("prospero-disk write probe, euid %d", static_cast<int>(geteuid()));

    // 1. Does the folder's owner decide, or the file's?
    const std::string base = data_root + "/probe-w";
    if (!exists(base))
        p.step("mkdir", base, app_user, [&] { return make_dir(base, 0777); });
    p.owner(base);
    folder_case(p, base, "root-0755", 0, 0755, 0);
    folder_case(p, base, "user-0700", app_user, 0700, app_user);
    folder_case(p, base, "root-0777", 0, 0777, 0);
    folder_case(p, base, "user-0755-rootfile", app_user, 0755, 0);
    p.step("rmdir", base, app_user, [&] { return remove_dir(base); });

    // 2. Throwaway entries where users keep things.
    const std::string stamp = std::to_string(static_cast<long long>(std::time(nullptr)));
    std::vector<std::string> targets = {"/data", "/user/download", "/user/data"};
    const std::string home = first_entry("/user/home");
    if (!home.empty())
        targets.push_back("/user/home/" + home);
    for (const char *drive : {"/mnt/ext0", "/mnt/ext1", "/mnt/usb0"})
    {
        if (mounted(drive))
            targets.emplace_back(drive);
        else
            p.line("skip %s: not mounted", drive);
    }
    for (const std::string &target : targets)
        location(p, target, stamp);

    // 3. Who owns what a user would collect (read only).
    list_owners(p, "/user/download", 8);
    if (!home.empty())
        list_owners(p, "/user/home/" + home, 8);
    list_owners(p, "/user/app", 3);
    list_owners(p, "/mnt/ext1/user/app", 3);
    list_owners(p, "/data/homebrew", 8);
    list_owners(p, "/data/playden/games", 3);

    p.line("write probe done, euid %d", static_cast<int>(geteuid()));
    return p.take();
}

} // namespace hui::ps5
