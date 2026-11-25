// Prospero Disk - Title IDs to names and icons, read off the frame thread (PRD 5.4).
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "disk/filesystem.hpp"
#include "disk/png.hpp"

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace hui::disk
{

// Four capitals and five digits: "PPSA01467", "CUSA00001".
bool is_title_id(std::string_view name);

// The title a folder is named after: the whole name when it is a title ID, or
// its first nine characters when a title ID is followed by '-', '_' or '.'
// ("PPSA28416-app0", "PPSA28416-patch0", "CUSA00001_00"); "" otherwise.
std::string_view title_id_of_folder(std::string_view name);

// Playden (a launcher that installs PC games) registers each game as its own
// PS5 title and writes <homebrew root>/<TITLE_ID>/.playden-title-owner,
// "key=value" lines with install_id (steam-2610, gog-1207658695,
// local-<uuid>) and title_id. Returns install_id -> title_id for every such
// folder; empty when there are none or the folder cannot be read.
std::map<std::string, std::string> read_playden_titles(const FileSystem &fs,
                                                       const std::string &homebrew_root);

// The Playden install a folder holds, for folders directly inside Playden's
// game folders ("<volume>/playden/games", "/data/playden/games" for bottles,
// "<volume>/playden-games"): "2610" -> "steam-2610"; "steam-...", "gog-..."
// and "local-..." names are kept. "" for any other folder.
std::string playden_install_id(std::string_view parent_path, std::string_view folder);

// The title's name in its default language from a param.json, or "".
std::string title_name_from_param_json(std::string_view json);

struct AppDbTitle
{
    std::string name;      // "" when app.db does not know the title
    std::string icon_path; // a readable icon0.png the console keeps, or ""
};

// The fallback (PRD 5.4) for titles whose /user/appmeta/<id> folder the app
// cannot open: retail games' folders are 0700 and the app acts as its own
// user after the grant. Reads the console's app.db, opened read-only and
// immutable (no locks, no journal). On PS5 firmware (seen on 13.60) the row is
// in tbl_contentinfo, whose icon0Info names a copy of the icon under
// /user/catalog_downloader that is readable ("...icon0.png?ts=..."; the query
// part is dropped). Older layouts keep the name in tbl_appbrowse_* tables.
AppDbTitle title_from_app_db(const std::string &path, std::string_view id);
std::string title_name_from_app_db(const std::string &path, std::string_view id);

struct TitleInfo
{
    std::string id;
    std::string name; // empty when no metadata was found
    Image icon;       // empty when there is no icon
};

// One background thread that reads appmeta/<id>/param.json and icon0.png.
//   loader.request("PPSA01467");   // any thread, once per id is enough
//   for (TitleInfo &t : loader.poll()) ...   // the frame thread
class TitleLoader
{
  public:
    // roots: folders holding <id>/param.json, best first
    // (/user/appmeta, /user/appmeta/external).
    // app_db: the console's app.db, asked when no param.json names the title.
    TitleLoader(const FileSystem &fs, std::vector<std::string> roots, int icon_size = 128,
                std::string app_db = {});
    ~TitleLoader();
    TitleLoader(const TitleLoader &) = delete;
    TitleLoader &operator=(const TitleLoader &) = delete;

    void request(std::string_view id);
    std::vector<TitleInfo> poll();
    // Blocks until every request so far is answered; for tests.
    void wait_idle();

  private:
    void run();
    TitleInfo load(const std::string &id) const;

    const FileSystem &fs_;
    std::vector<std::string> roots_;
    int icon_size_;
    std::string app_db_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::string> queue_;
    std::vector<std::string> requested_;
    std::vector<TitleInfo> done_;
    bool working_ = false;
    bool stop_ = false;
    std::thread thread_;
};

} // namespace hui::disk
