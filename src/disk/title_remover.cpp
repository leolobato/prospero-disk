// Prospero Disk - Uninstalling a title through the system, and the fixture's stand-in.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "disk/title_remover.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace hui::disk
{

namespace
{

constexpr std::size_t kMaxPayload = 4u * 1024u * 1024u;

// The whole file, without following a link; false when it is not a regular
// file of at most limit bytes.
bool read_no_follow(const std::string &path, std::size_t limit, std::string *out)
{
    const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return false;
    struct stat info
    {
    };
    bool ok = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
              static_cast<std::size_t>(info.st_size) <= limit;
    if (ok)
        out->resize(static_cast<std::size_t>(info.st_size));
    std::size_t done = 0;
    while (ok && done < out->size())
    {
        const ssize_t count = read(fd, out->data() + done, out->size() - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            ok = false;
        else
            done += static_cast<std::size_t>(count);
    }
    close(fd);
    return ok;
}

// The request, written whole under another name and renamed: the payload
// never reads half of one.
bool write_request(const std::string &dir, const std::string &text)
{
    const std::string temporary = dir + "/request.tmp";
    unlink(temporary.c_str());
    const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0666);
    if (fd < 0)
        return false;
    bool ok = fchmod(fd, 0666) == 0 &&
              write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    ok = close(fd) == 0 && ok;
    if (!ok || rename(temporary.c_str(), (dir + "/request").c_str()) != 0)
    {
        unlink(temporary.c_str());
        return false;
    }
    return true;
}

// The folder the payload trusts: the app's own (not root's), not a link.
bool ready_folder(const std::string &dir)
{
    struct stat info
    {
    };
    if (lstat(dir.c_str(), &info) != 0)
    {
        if (errno != ENOENT || mkdir(dir.c_str(), 0777) != 0 || lstat(dir.c_str(), &info) != 0)
            return false;
    }
    if (!S_ISDIR(info.st_mode) || info.st_uid != geteuid() || info.st_uid == 0)
        return false;
    return (info.st_mode & 07777) == 0777 || chmod(dir.c_str(), 0777) == 0;
}

std::string hex_code(unsigned code)
{
    char text[16];
    std::snprintf(text, sizeof text, "0x%08x", code);
    return text;
}

void remove_tree(const FileSystem &fs, const std::string &path)
{
    std::vector<Entry> entries;
    int error = 0;
    if (!fs.list(path, &entries, &error))
        return;
    for (const Entry &e : entries)
    {
        const std::string child = path + "/" + e.name;
        if (e.type == EntryType::directory)
            remove_tree(fs, child);
        else
            fs.remove_file(child, &error);
    }
    fs.remove_folder(path, &error);
}

class FixtureTitleRemover final : public TitleRemover
{
  public:
    FixtureTitleRemover(const FileSystem &fs, std::string root) : fs_(fs), root_(std::move(root))
    {
    }
    UninstallResult uninstall(const std::string &title_id) override
    {
        UninstallResult r;
        Entry app;
        if (!fs_.stat(root_ + "/app/" + title_id, &app))
        {
            r.reason = "The console does not know this title.";
            return r;
        }
        for (const char *folder : {"/app/", "/patch/", "/appmeta/"})
            remove_tree(fs_, root_ + folder + title_id);
        r.ok = true;
        return r;
    }

  private:
    const FileSystem &fs_;
    std::string root_;
};

} // namespace

UninstallResult PayloadTitleRemover::uninstall(const std::string &title_id)
{
    UninstallResult r;
    const std::string &dir = config_.request_dir;
    const TitlePresence before =
        config_.presence ? config_.presence(title_id) : TitlePresence::unknown;
    if (before != TitlePresence::listed)
    {
        r.reason = before == TitlePresence::gone ? "The console does not have this title."
                                                 : "Prospero Disk could not check this title.";
        return r;
    }
    if (!ready_folder(dir))
    {
        r.reason = "The title remover's folder is not Prospero Disk's.";
        return r;
    }
    // 16 or more of [0-9a-f-]: the answer must carry it back.
    char nonce[48];
    std::snprintf(nonce, sizeof nonce, "%016llx-%04x",
                  static_cast<unsigned long long>(
                      std::chrono::steady_clock::now().time_since_epoch().count()),
                  ++serial_ & 0xffffu);
    unlink((dir + "/result").c_str());
    unlink((dir + "/result.tmp").c_str());
    if (!write_request(dir, "title=" + title_id + "\nnonce=" + nonce + "\n"))
    {
        r.reason = "The uninstall request could not be written.";
        return r;
    }
    std::string elf;
    std::string error;
    bool sent = false;
    if (!read_no_follow(config_.payload, kMaxPayload, &elf) || elf.size() < 4 ||
        elf.compare(0, 4,
                    "\x7f"
                    "ELF") != 0)
        r.reason = "The title remover is missing from the install folder.";
    else if (!config_.send || !(sent = config_.send(elf, &error)))
        r.reason =
            "The payload loader on port 9021 did not take the title remover (" + error + ").";
    long code = -1;
    // The answer names the title and the nonce: an old answer, or one for
    // another title, is not taken.
    const std::string expected = "title=" + title_id + "\nnonce=" + nonce + "\n";
    for (int poll = 0; sent && poll < config_.answer_polls && code < 0; ++poll)
    {
        std::string text;
        if (read_no_follow(dir + "/result", 256, &text) && text.rfind(expected, 0) == 0)
        {
            const std::size_t at = text.find("code=");
            if (at != std::string::npos)
                code = static_cast<long>(std::strtoul(text.c_str() + at + 5, nullptr, 16));
        }
        if (code < 0 && config_.sleep)
            config_.sleep(config_.answer_ms);
    }
    unlink((dir + "/request").c_str());
    if (!sent)
        return r;
    if (code < 0)
    {
        r.reason = "The title remover did not answer.";
        return r;
    }
    if (code != 0)
    {
        r.reason =
            "The console refused to uninstall it (" + hex_code(static_cast<unsigned>(code)) + ").";
        return r;
    }
    // Accepted: the system finishes on its own time.
    r.accepted = true;
    for (int poll = 0; poll < config_.confirm_polls; ++poll)
    {
        if (config_.presence(title_id) == TitlePresence::gone)
        {
            r.ok = true;
            return r;
        }
        if (config_.sleep)
            config_.sleep(config_.confirm_ms);
    }
    r.reason = "The console has not confirmed the uninstall yet. Rescan later to check.";
    return r;
}

bool send_to_payload_loader(const std::string &elf, std::string *error)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        *error = "socket " + std::to_string(errno);
        return false;
    }
    struct timeval timeout
    {
        10, 0
    };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    struct sockaddr_in address
    {
    };
    address.sin_family = AF_INET;
    address.sin_port = htons(9021);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<struct sockaddr *>(&address), sizeof address) != 0)
    {
        *error = "connect " + std::to_string(errno);
        close(fd);
        return false;
    }
    std::size_t done = 0;
    while (done < elf.size())
    {
        const ssize_t count = send(fd, elf.data() + done, elf.size() - done, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
        {
            *error = "send " + std::to_string(errno);
            close(fd);
            return false;
        }
        done += static_cast<std::size_t>(count);
    }
    shutdown(fd, SHUT_WR);
    close(fd);
    return true;
}

std::unique_ptr<TitleRemover> make_fixture_title_remover(const FileSystem &fs, std::string root)
{
    return std::make_unique<FixtureTitleRemover>(fs, std::move(root));
}

} // namespace hui::disk
