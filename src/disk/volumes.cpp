// Prospero Disk - Which volumes exist, and the environment the app scans them in.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "disk/volumes.hpp"

#include "disk/title_remover.hpp"
#include "disk/titles.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <thread>
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

ProtectionRules console_protection(const std::vector<VolumeSpec> &specs,
                                   std::map<std::string, std::string> playden_titles)
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
    };
    rules.title_parents = {"/user/app", "/mnt/ext0/user/app", "/mnt/ext1/user/app"};
    rules.title_companions = {"/user/patch",          "/user/addcont",
                              "/mnt/ext0/user/patch", "/mnt/ext0/user/addcont",
                              "/mnt/ext1/user/patch", "/mnt/ext1/user/addcont"};
    rules.aliases = {{"/user/data", "/data"}};
    rules.homebrew = "/data/homebrew";
    rules.playden_game_folders = {"/data/playden/games"};
    for (const VolumeSpec &spec : specs)
        if (spec.path != "/data")
        {
            rules.playden_game_folders.push_back(spec.path + "/playden/games");
            rules.playden_game_folders.push_back(spec.path + "/playden-games");
        }
    rules.playden_titles = std::move(playden_titles);
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
    env.protection = console_protection(env.specs, env.playden_titles);
    env.privileged = privileged_state();
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
