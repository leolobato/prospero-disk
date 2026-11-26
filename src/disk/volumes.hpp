// Prospero Disk - Which volumes exist, and the environment the app scans them in.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "disk/collection.hpp"
#include "disk/filesystem.hpp"

#include <cstdint>
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
    std::string cache_dir;      // empty: no cache
    Elevation elevation;
    bool fixture = false;
};

// The console's real volumes on a PS5. On a PC: the folders in PD_VOLUMES
// ("Name=/path;Other=/path"), else the built-in fixture.
Environment make_environment();

// The console's rules: what Prospero Disk never deletes, where installed
// titles are, the /user/data alias, and Playden's titles.
ProtectionRules console_protection(const std::vector<VolumeSpec> &specs,
                                   std::map<std::string, std::string> playden_titles);

// Set once by main() on the console, before the first design is created.
void set_elevation(const Elevation &elevation);

} // namespace hui::disk
