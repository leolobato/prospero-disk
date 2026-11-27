// Prospero Disk - Which volumes exist, and the environment the app scans them in.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "disk/collection.hpp"
#include "disk/deleter.hpp"
#include "disk/filesystem.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hui::disk
{

enum class VolumeKind : std::uint8_t
{
    internal,
    extended, // M.2 / extended storage
    usb,
    folder, // a folder named on the PC (PD_VOLUMES)
};

struct VolumeSpec
{
    std::string key;  // cache file name: "internal", "usb0"
    std::string name; // "Internal", "USB 1"
    std::string path;
    VolumeKind kind = VolumeKind::internal;
    // The physical drive, for collection totals: "" is the volume's own key.
    // /data is a view of /user/data, so the console's "data" volume is "internal".
    std::string drive;
};

// The drive a volume is on (VolumeSpec::drive, else its key).
inline const std::string &drive_of(const VolumeSpec &spec)
{
    return spec.drive.empty() ? spec.key : spec.drive;
}

struct Volume
{
    VolumeSpec spec;
    SpaceInfo space;
    bool has_space = false; // statfs answered
};

// The candidates on a console (PRD F1). Which are real is decided by
// detect_volumes; /user is "Internal" until the mount spike (PRD 10.2) says
// otherwise.
std::vector<VolumeSpec> console_volume_specs();

// Keeps the candidates whose folder can be listed, with their space.
std::vector<Volume> detect_volumes(const FileSystem &fs, const std::vector<VolumeSpec> &specs);

// Scanner threads for a kind of volume (PRD 7.3).
int default_threads(VolumeKind kind);

// What the sandbox elevation achieved, for the degraded-mode banner (PRD 5.1).
struct Elevation
{
    bool attempted = false;
    bool granted = false;
    std::string diagnostic; // "helper stage=connect status=unavailable error=61"
};

struct Environment
{
    std::unique_ptr<FileSystem> fs;
    std::vector<VolumeSpec> specs;
    std::vector<std::string> appmeta_roots; // where <id>/param.json lives
    std::string app_db;                     // the console's app.db (name fallback)
    // Playden's games: install id (steam-2610) -> its PS5 title (FAKE78024).
    std::map<std::string, std::string> playden_titles;
    ProtectionRules protection; // what may be collected (docs/plans/collect-delete.md)
    Privileged privileged;      // raises the euid for one removal; empty off the console
    // Uninstalls collected games: the payload on a PS5, a stand-in on the
    // fixture, none for folders on a PC.
    std::shared_ptr<TitleRemover> remover;
    std::string trash_dir; // DeleteOptions::trash_dir
    // DeleteOptions::forget_playden_game: a line in Playden's request file.
    std::function<bool(const std::string &)> forget_playden_game;
    // DeleteOptions::read_shadowmount_sources.
    std::function<std::map<std::string, std::string>()> read_shadowmount_sources;
    std::string cache_dir; // empty: no cache
    Elevation elevation;
    bool fixture = false;
};

// The console's real volumes on a PS5. On a PC: the folders in PD_VOLUMES
// ("Name=/path;Other=/path"), else the built-in fixture.
Environment make_environment();

// The console's rules: what Prospero Disk never deletes, where installed
// titles are, the /user/data alias, and Playden's titles.
ProtectionRules
console_protection(const std::vector<VolumeSpec> &specs,
                   std::map<std::string, std::string> playden_titles,
                   const std::map<std::string, std::string> &shadowmount_sources = {},
                   std::vector<std::string> playden_copies = {});

// Playden install ids with a game folder in more than one of the given
// playden-games folders (a copy left on another drive).
std::vector<std::string> read_playden_copies(const FileSystem &fs, const Privileged &privileged,
                                             const std::vector<std::string> &folders);

// ShadowMountPlus titles in the given /user/app folders: each title's
// mount.lnk names the folder it is installed from. A title with no mount.lnk
// (a package install) is left out; one whose link can't be read, isn't a
// plain path into a "homebrew" folder, names a folder whose param.json is
// not this title's, or differs between two app folders maps to "", which the
// rules refuse.
std::map<std::string, std::string>
read_shadowmount_sources(const FileSystem &fs, const Privileged &privileged,
                         const std::vector<std::string> &parents);

// Adds "uninstall <game>" to Playden's request file (<title_root>/dev/
// install-request.txt), which Playden reads on its next start. False for a
// game word Playden would not take, or when the line could not be written.
bool append_playden_request(const std::string &title_root, const std::string &game,
                            const Privileged &privileged);

// Set once by main() on the console, before the first design is created.
void set_elevation(const Elevation &elevation);
void set_privileged(Privileged privileged);

} // namespace hui::disk
