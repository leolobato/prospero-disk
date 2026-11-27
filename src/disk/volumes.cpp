// Prospero Disk - Which volumes exist, and the environment the app scans them in.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "disk/volumes.hpp"

#include "disk/title_remover.hpp"
#include "disk/titles.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace hui::disk
{

namespace
{

Elevation &elevation_state()
{
    static Elevation state;
    return state;
}

// A path ShadowMount may install a title from: absolute, plain, and a
// folder directly inside a "homebrew" folder (where it scans).
bool plain_source(const std::string &path)
{
    if (path.size() < 2 || path.front() != '/' || path.find("//") != std::string::npos ||
        path.find("/../") != std::string::npos || path.find("/./") != std::string::npos ||
        path.ends_with("/..") || path.ends_with("/.") || path.ends_with("/"))
        return false;
    for (const char c : path)
        if (static_cast<unsigned char>(c) < 0x20)
            return false;
    const std::size_t slash = path.find_last_of('/');
    const std::string parent = path.substr(0, slash);
    return parent.ends_with("/homebrew");
}

// Reads a whole folder, raised when the app's user may not (the M.2 app
// folders are root's 0770).
bool list_raised(const FileSystem &fs, const Privileged &privileged, const std::string &path,
                 std::vector<Entry> *out)
{
    int error = 0;
    if (fs.list(path, out, &error))
        return true;
    bool ok = false;
    if ((error == EACCES || error == EPERM) && privileged)
        privileged(
            [&]
            {
                out->clear();
                ok = fs.list(path, out, &error);
                return ok;
            });
    return ok;
}

bool read_raised(const FileSystem &fs, const Privileged &privileged, const std::string &path,
                 std::string *out, std::size_t limit = 1024)
{
    if (fs.read_file(path, out, limit))
        return true;
    bool ok = false;
    if ((errno == EACCES || errno == EPERM) && privileged)
        privileged(
            [&]
            {
                ok = fs.read_file(path, out, limit);
                return ok;
            });
    return ok;
}

// lstat, raised when the app's user may not look; errno says why it failed.
bool stat_raised(const FileSystem &fs, const Privileged &privileged, const std::string &path,
                 Entry *out)
{
    if (fs.stat(path, out))
        return true;
    int error = errno;
    bool ok = false;
    if ((error == EACCES || error == EPERM) && privileged)
        privileged(
            [&]
            {
                ok = fs.stat(path, out);
                error = ok ? 0 : errno;
                return ok;
            });
    errno = error;
    return ok;
}

// A folder that must be a real folder, not a link to one.
bool real_folder(const std::string &path)
{
    struct stat st
    {
    };
    return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// What Playden's uninstall request accepts: a Steam app ID, gog-<id> or local-<uuid>.
bool playden_request_word(const std::string &word)
{
    const auto digits = [](std::string_view s)
    { return !s.empty() && s.size() <= 20 && s.find_first_not_of("0123456789") == s.npos; };
    if (digits(word))
        return true;
    if (word.starts_with("gog-"))
        return digits(std::string_view(word).substr(4));
    if (word.starts_with("local-"))
        return word.size() <= 64 &&
               word.find_first_not_of("0123456789abcdef-", 6) == std::string::npos;
    return false;
}

Privileged &privileged_state()
{
    static Privileged state;
    return state;
}

#if !defined(__PROSPERO__)
std::vector<VolumeSpec> parse_volumes(const std::string &text)
{
    std::vector<VolumeSpec> out;
    std::size_t at = 0;
    while (at < text.size())
    {
        std::size_t end = text.find(';', at);
        if (end == std::string::npos)
            end = text.size();
        const std::string item = text.substr(at, end - at);
        const std::size_t eq = item.find('=');
        if (eq != std::string::npos && eq + 1 < item.size())
        {
            VolumeSpec spec;
            spec.name = item.substr(0, eq);
            spec.path = item.substr(eq + 1);
            spec.key = "folder" + std::to_string(out.size());
            spec.kind = VolumeKind::folder;
            out.push_back(spec);
        }
        at = end + 1;
    }
    return out;
}
#endif

// A drive's folder under /mnt exists whether or not a drive is mounted on
// it (on the console /mnt/ext0 and /mnt/usb2-7 are listable, empty folders of
// /mnt's own file system). Only a folder on another device than its parent
// is a mounted drive. When either lstat fails, the folder is kept.
bool is_mounted(const FileSystem &fs, const VolumeSpec &spec)
{
    if (spec.kind != VolumeKind::extended && spec.kind != VolumeKind::usb)
        return true;
    const std::size_t slash = spec.path.find_last_of('/');
    if (slash == std::string::npos || slash == 0)
        return true;
    Entry self;
    Entry parent;
    if (!fs.stat(spec.path, &self) || !fs.stat(spec.path.substr(0, slash), &parent))
        return true;
    return self.device != parent.device;
}

} // namespace

std::vector<VolumeSpec> console_volume_specs()
{
    // /data is not listed: on the console it is a nullfs view of /user/data, so it
    // would repeat Internal's disk bar and count files a scan of /user already counts.
    std::vector<VolumeSpec> specs = {
        {"internal", "Internal", "/user", VolumeKind::internal, ""},
        {"ext0", "Extended", "/mnt/ext0", VolumeKind::extended, ""},
        {"ext1", "Extended 2", "/mnt/ext1", VolumeKind::extended, ""},
    };
    for (int i = 0; i < 8; ++i)
        specs.push_back({"usb" + std::to_string(i), "USB " + std::to_string(i + 1),
                         "/mnt/usb" + std::to_string(i), VolumeKind::usb, ""});
    return specs;
}

std::vector<Volume> detect_volumes(const FileSystem &fs, const std::vector<VolumeSpec> &specs)
{
    std::vector<Volume> out;
    std::vector<Entry> entries;
    for (const VolumeSpec &spec : specs)
    {
        int error = 0;
        if (!fs.list(spec.path, &entries, &error))
            continue;
        if (!is_mounted(fs, spec))
            continue;
        Volume v;
        v.spec = spec;
        v.has_space = fs.space(spec.path, &v.space);
        out.push_back(v);
    }
    return out;
}

std::map<std::string, std::string> read_shadowmount_sources(const FileSystem &fs,
                                                            const Privileged &privileged,
                                                            const std::vector<std::string> &parents)
{
    std::map<std::string, std::string> sources;
    for (const std::string &parent : parents)
    {
        std::vector<Entry> titles;
        if (!list_raised(fs, privileged, parent, &titles))
            continue;
        for (const Entry &title : titles)
        {
            if (title.type != EntryType::directory ||
                (!is_title_id(title.name) && !title.name.starts_with("FAKE78")))
                continue;
            const std::string link = parent + "/" + title.name + "/mount.lnk";
            Entry there;
            if (!stat_raised(fs, privileged, link, &there) && (errno == ENOENT || errno == ENOTDIR))
                continue; // a package install: the system owns its folders
            // From here on, anything that doesn't add up is kept as "" so the
            // rules refuse the title rather than treat it as a package.
            std::string text;
            if (!read_raised(fs, privileged, link, &text))
                text.clear();
            // Only the line end: a trailing space is part of a folder's name.
            while (!text.empty() &&
                   (text.back() == '\n' || text.back() == '\r' || text.back() == '\0'))
                text.pop_back();
            std::string param;
            if (!plain_source(text) ||
                !read_raised(fs, privileged, text + "/sce_sys/param.json", &param, 256 * 1024) ||
                param.find(title.name) == std::string::npos)
                text.clear(); // not the folder of this title
            const auto known = sources.find(title.name);
            if (known != sources.end() && known->second != text)
                known->second.clear(); // one title, two folders
            else
                sources[title.name] = text;
        }
    }
    return sources;
}

std::vector<std::string> read_playden_copies(const FileSystem &fs, const Privileged &privileged,
                                             const std::vector<std::string> &folders)
{
    std::map<std::string, int> seen;
    for (const std::string &folder : folders)
    {
        std::vector<Entry> games;
        if (!list_raised(fs, privileged, folder, &games))
            continue;
        for (const Entry &game : games)
        {
            const std::string id = playden_install_id(folder, game.name);
            if (game.type == EntryType::directory && !id.empty())
                ++seen[id];
        }
    }
    std::vector<std::string> copies;
    for (const auto &[id, count] : seen)
        if (count > 1)
            copies.push_back(id);
    return copies;
}

bool append_playden_request(const std::string &title_root, const std::string &game,
                            const Privileged &privileged)
{
    if (!playden_request_word(game))
        return false;
    const std::string dev = title_root + "/dev";
    const std::string line = "uninstall " + game + "\n";
    const auto write_line = [&]
    {
        // Real folders all the way, and a plain file with one name: a link
        // anywhere would send the line (and the chmod) somewhere else.
        if (!real_folder(title_root))
            return false;
        if (mkdir(dev.c_str(), 0777) != 0 && errno != EEXIST)
            return false;
        if (!real_folder(dev))
            return false;
        const int fd = open((dev + "/install-request.txt").c_str(),
                            O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW, 0666);
        if (fd < 0)
            return false;
        struct stat st
        {
        };
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1)
        {
            close(fd);
            return false;
        }
        fchmod(fd, 0666); // Playden runs as another user and deletes it when read
        const bool ok = write(fd, line.data(), line.size()) == static_cast<ssize_t>(line.size());
        return close(fd) == 0 && ok;
    };
    if (write_line())
        return true;
    bool ok = false;
    if (privileged)
        privileged(
            [&]
            {
                ok = write_line();
                return ok;
            });
    return ok;
}

ProtectionRules console_protection(const std::vector<VolumeSpec> &specs,
                                   std::map<std::string, std::string> playden_titles,
                                   const std::map<std::string, std::string> &shadowmount_sources,
                                   std::vector<std::string> playden_copies)
{
    ProtectionRules rules;
    for (const VolumeSpec &spec : specs)
        rules.volume_roots.push_back(spec.path);
    // /data has no row of its own now, but it is still a mount (a view of
    // /user/data): never collectable, and the root of what is under it.
    rules.volume_roots.emplace_back("/data");
    const std::string system = "Part of the console's system: it must stay.";
    const std::string meta = "The console's game information: it must stay.";
    const std::string own = "Prospero Disk's own files can't be collected.";
    rules.protected_roots = {
        {"/system*", system},
        {"/preinst", system},
        {"/user/appmeta", meta},
        {"/mnt/ext0/user/appmeta", meta},
        {"/mnt/ext1/user/appmeta", meta},
        {"/data/homebrew/PPSA93475", own},
        {"/user/app/PPSA93475", own}, // the console's registration of Prospero Disk
        {"/data/prospero-disk", own},
        {"/data/homebrew/FAKE77031", "Uninstall Playden from its own settings."},
        {"/data/homebrew/FAKE10101", "Part of ShadowMountPlus: it must stay."},
    };
    rules.title_parents = {"/user/app", "/mnt/ext0/user/app", "/mnt/ext1/user/app"};
    rules.title_companions = {"/user/patch",          "/user/addcont",
                              "/mnt/ext0/user/patch", "/mnt/ext0/user/addcont",
                              "/mnt/ext1/user/patch", "/mnt/ext1/user/addcont"};
    rules.aliases = {{"/user/data", "/data"}};
    rules.homebrew = "/data/homebrew";
    // Playden's bottles and, since 0.6, its game files; /data is not a
    // volume of its own, so neither comes from the loop below.
    rules.playden_game_folders = {"/data/playden/games", "/data/playden-games"};
    for (const VolumeSpec &spec : specs)
        if (spec.path != "/data")
        {
            rules.playden_game_folders.push_back(spec.path + "/playden/games");
            rules.playden_game_folders.push_back(spec.path + "/playden-games");
        }
    rules.playden_titles = std::move(playden_titles);
    rules.playden_copies = std::move(playden_copies);
    // Where ShadowMount looks: a source must be a folder directly in one, not
    // another title's folder, and no other title's source.
    std::vector<std::string> scanned = {"/data/homebrew"};
    for (const VolumeSpec &spec : specs)
        scanned.push_back(spec.path + "/homebrew");
    std::map<std::string, std::vector<std::string>> by_source;
    for (const auto &[id, raw] : shadowmount_sources)
    {
        if (raw.empty())
        {
            rules.shadowmount_refused.push_back(id);
            continue;
        }
        const std::string source = canonical_path(rules, raw);
        const std::size_t slash = source.find_last_of('/');
        const std::string folder = source.substr(0, slash);
        const std::string name = source.substr(slash + 1);
        if (std::find(scanned.begin(), scanned.end(), folder) == scanned.end() ||
            (is_title_id(name) && name != id))
        {
            rules.shadowmount_refused.push_back(id);
            continue;
        }
        by_source[source].push_back(id);
    }
    for (const auto &[source, ids] : by_source)
    {
        if (ids.size() == 1)
        {
            rules.shadowmount_sources[ids.front()] = source;
            continue;
        }
        rules.shadowmount_refused.push_back(source);
        rules.shadowmount_refused.insert(rules.shadowmount_refused.end(), ids.begin(), ids.end());
    }
    return rules;
}

int default_threads(VolumeKind kind)
{
    return kind == VolumeKind::usb ? 2 : 8;
}

void set_elevation(const Elevation &elevation)
{
    elevation_state() = elevation;
}

void set_privileged(Privileged privileged)
{
    privileged_state() = std::move(privileged);
}

Environment make_environment()
{
    Environment env;
    env.elevation = elevation_state();
#if defined(__PROSPERO__)
    env.fs = make_posix_filesystem(env.elevation.granted, env.elevation.granted);
    env.specs = console_volume_specs();
    env.appmeta_roots = {"/user/appmeta", "/user/appmeta/external"};
    env.app_db = "/system_data/priv/mms/app.db";
    if (env.elevation.granted)
        env.playden_titles = read_playden_titles(*env.fs, "/data/homebrew");
    // /data needs the elevation; without it the title keeps its own storage.
    env.cache_dir =
        env.elevation.granted ? "/data/prospero-disk/cache" : "/download0/prospero-disk/cache";
    env.privileged = privileged_state();
    std::map<std::string, std::string> sources;
    std::vector<std::string> copies;
    const std::vector<std::string> app_roots = {"/user/app", "/mnt/ext0/user/app",
                                                "/mnt/ext1/user/app"};
    if (env.elevation.granted)
    {
        sources = read_shadowmount_sources(*env.fs, env.privileged, app_roots);
        std::vector<std::string> games = {"/data/playden-games"};
        for (const VolumeSpec &spec : env.specs)
            if (spec.path != "/user")
                games.push_back(spec.path + "/playden-games");
        copies = read_playden_copies(*env.fs, env.privileged, games);
    }
    env.protection = console_protection(env.specs, env.playden_titles, sources, copies);
    if (env.elevation.granted)
    {
        TitleRemoverConfig remover;
        remover.payload = "/data/homebrew/PPSA93475/title-remover.elf";
        remover.send = send_to_payload_loader;
        // As Playden checks: the title is there while its app folder is. The
        // M.2 app folders are root's 0770, so a refusal is asked again raised.
        const FileSystem *fs = env.fs.get();
        const Privileged privileged = env.privileged;
        remover.presence = [fs, privileged](const std::string &id)
        {
            bool unknown = false;
            for (const char *root : {"/user/app/", "/mnt/ext0/user/app/", "/mnt/ext1/user/app/"})
            {
                const std::string path = root + id;
                Entry e;
                if (fs->stat(path, &e))
                    return TitlePresence::listed;
                int error = errno;
                if ((error == EACCES || error == EPERM) && privileged)
                {
                    bool found = false;
                    privileged(
                        [&]
                        {
                            found = fs->stat(path, &e);
                            error = found ? 0 : errno;
                            return found;
                        });
                    if (found)
                        return TitlePresence::listed;
                }
                // The M.2 folders exist without a drive: ENOENT there is "not here".
                if (error != ENOENT && error != ENOTDIR)
                    unknown = true;
            }
            return unknown ? TitlePresence::unknown : TitlePresence::gone;
        };
        remover.sleep = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
        env.remover = std::make_shared<PayloadTitleRemover>(std::move(remover));
        env.trash_dir = "/data/prospero-disk/trash"; // made by main()
        const Privileged raised = env.privileged;
        env.forget_playden_game = [raised](const std::string &game)
        { return append_playden_request("/data/homebrew/FAKE77031", game, raised); };
        // Read again at delete time: a title may have moved since startup.
        env.read_shadowmount_sources = [fs, raised, app_roots]
        { return read_shadowmount_sources(*fs, raised, app_roots); };
    }
#else
    const char *volumes = std::getenv("PD_VOLUMES");
    if (volumes != nullptr && volumes[0] != 0)
    {
        // Real folders on a PC: deleting from them is opt-in.
        const char *allow = std::getenv("PD_ALLOW_DELETE");
        env.fs = make_posix_filesystem(true, allow != nullptr && std::string(allow) == "1");
        env.specs = parse_volumes(volumes);
        if (const char *cache = std::getenv("PD_CACHE"); cache != nullptr)
            env.cache_dir = cache;
        if (const char *db = std::getenv("PD_APP_DB"); db != nullptr)
            env.app_db = db;
        if (const char *homebrew = std::getenv("PD_HOMEBREW"); homebrew != nullptr)
            env.playden_titles = read_playden_titles(*env.fs, homebrew);
        // Folders named on a PC: only the volumes themselves are refused.
        for (const VolumeSpec &spec : env.specs)
            env.protection.volume_roots.push_back(spec.path);
    }
    else
    {
        // Paced by the screen's clock, so the tour photographs a scan in progress.
        env.fs = make_fixture_filesystem(45.0);
        env.specs = {{"internal", "Internal", kFixtureRoot, VolumeKind::internal, ""},
                     {"usb0", "USB 1", kFixtureUsbRoot, VolumeKind::usb, ""}};
        env.appmeta_roots = {std::string(kFixtureRoot) + "/appmeta"};
        env.fixture = true;
        // The console's rules on the fixture's layout.
        const std::string root = kFixtureRoot;
        env.protection.volume_roots = {kFixtureRoot, kFixtureUsbRoot};
        env.protection.protected_roots = {
            {root + "/appmeta", "The console's game information: it must stay."}};
        env.protection.title_parents = {root + "/app"};
        env.protection.title_companions = {root + "/patch"};
        env.remover = make_fixture_title_remover(*env.fs, root);
    }
#endif
    return env;
}

} // namespace hui::disk
