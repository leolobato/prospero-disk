// Prospero Disk - Title IDs to names and icons, read off the frame thread (PRD 5.4).
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "disk/filesystem.hpp"
#include "disk/png.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace hui::disk
{

// Four capitals and five digits: "PPSA01467", "CUSA00001".
bool is_title_id(std::string_view name);

// The title's name in its default language from a param.json, or "".
std::string title_name_from_param_json(std::string_view json);

// The fallback (PRD 5.4): titleName for the ID in any tbl_appbrowse_* table
// of the console's app.db, opened read-only and immutable (no locks, no
// journal); "" when there is none.
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
