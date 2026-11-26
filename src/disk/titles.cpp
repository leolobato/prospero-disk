// Prospero Disk - Title IDs to names and icons, read off the frame thread (PRD 5.4).
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "disk/titles.hpp"

#include "disk/sqlite.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>

namespace hui::disk
{

bool is_title_id(std::string_view name)
{
    if (name.size() != 9)
        return false;
    for (std::size_t i = 0; i < 4; ++i)
        if (name[i] < 'A' || name[i] > 'Z')
            return false;
    for (std::size_t i = 4; i < 9; ++i)
        if (name[i] < '0' || name[i] > '9')
            return false;
    return true;
}

std::string_view title_id_of_folder(std::string_view name)
{
    if (name.size() < 9 || !is_title_id(name.substr(0, 9)))
        return {};
    if (name.size() == 9 || name[9] == '-' || name[9] == '_' || name[9] == '.')
        return name.substr(0, 9);
    return {};
}

std::map<std::string, std::string> read_playden_titles(const FileSystem &fs,
                                                       const std::string &homebrew_root)
{
    std::map<std::string, std::string> out;
    std::vector<Entry> entries;
    int error = 0;
    if (!fs.list(homebrew_root, &entries, &error))
        return out;
    for (const Entry &e : entries)
    {
        if (e.type != EntryType::directory || !is_title_id(e.name))
            continue;
        std::string text;
        if (!fs.read_file(homebrew_root + "/" + e.name + "/.playden-title-owner", &text, 4096))
            continue;
        std::string install;
        std::string title;
        std::size_t at = 0;
        while (at < text.size())
        {
            std::size_t end = text.find('\n', at);
            if (end == std::string::npos)
                end = text.size();
            const std::string_view line(text.data() + at, end - at);
            if (line.starts_with("install_id="))
                install = std::string(line.substr(11));
            else if (line.starts_with("title_id="))
                title = std::string(line.substr(9));
            at = end + 1;
        }
        while (!install.empty() && (install.back() == '\r' || install.back() == ' '))
            install.pop_back();
        while (!title.empty() && (title.back() == '\r' || title.back() == ' '))
            title.pop_back();
        // The folder must be the title it names: a copied file is not believed.
        if (!install.empty() && title == e.name)
            out[install] = title;
    }
    return out;
}

std::string playden_install_id(std::string_view parent_path, std::string_view folder)
{
    if (!parent_path.ends_with("/playden/games") && !parent_path.ends_with("/playden-games"))
        return {};
    if (folder.empty())
        return {};
    if (folder.starts_with("steam-") || folder.starts_with("gog-") || folder.starts_with("local-"))
        return std::string(folder);
    for (const char c : folder)
        if (c < '0' || c > '9')
            return {};
    return "steam-" + std::string(folder);
}

namespace
{

// Just enough JSON for param.json: objects, strings, and everything else skipped.
struct Json
{
    std::string text;                                  // a string value
    std::map<std::string, std::unique_ptr<Json>> keys; // an object's members
    bool is_object = false;
    bool is_string = false;
};

class Parser
{
  public:
    explicit Parser(std::string_view s) : s_(s)
    {
    }

    bool value(Json *out, int depth = 0)
    {
        if (depth > 32)
            return false;
        space();
        if (at_ >= s_.size())
            return false;
        const char c = s_[at_];
        if (c == '{')
            return object(out, depth);
        if (c == '[')
            return array(depth);
        if (c == '"')
        {
            out->is_string = true;
            return string(&out->text);
        }
        // Numbers, true, false, null: skip to the next delimiter.
        while (at_ < s_.size() && s_[at_] != ',' && s_[at_] != '}' && s_[at_] != ']')
            ++at_;
        return true;
    }

  private:
    void space()
    {
        while (at_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[at_])) != 0)
            ++at_;
    }

    bool object(Json *out, int depth)
    {
        out->is_object = true;
        ++at_;
        space();
        if (at_ < s_.size() && s_[at_] == '}')
        {
            ++at_;
            return true;
        }
        for (;;)
        {
            space();
            std::string key;
            if (at_ >= s_.size() || s_[at_] != '"' || !string(&key))
                return false;
            space();
            if (at_ >= s_.size() || s_[at_] != ':')
                return false;
            ++at_;
            auto member = std::make_unique<Json>();
            if (!value(member.get(), depth + 1))
                return false;
            out->keys[key] = std::move(member);
            space();
            if (at_ < s_.size() && s_[at_] == ',')
            {
                ++at_;
                continue;
            }
            if (at_ < s_.size() && s_[at_] == '}')
            {
                ++at_;
                return true;
            }
            return false;
        }
    }

    bool array(int depth)
    {
        ++at_;
        space();
        if (at_ < s_.size() && s_[at_] == ']')
        {
            ++at_;
            return true;
        }
        for (;;)
        {
            Json ignored;
            if (!value(&ignored, depth + 1))
                return false;
            space();
            if (at_ < s_.size() && s_[at_] == ',')
            {
                ++at_;
                continue;
            }
            if (at_ < s_.size() && s_[at_] == ']')
            {
                ++at_;
                return true;
            }
            return false;
        }
    }

    static void utf8(std::string *out, unsigned code)
    {
        if (code < 0x80)
            out->push_back(static_cast<char>(code));
        else if (code < 0x800)
        {
            out->push_back(static_cast<char>(0xc0 | (code >> 6)));
            out->push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
        else if (code < 0x10000)
        {
            out->push_back(static_cast<char>(0xe0 | (code >> 12)));
            out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out->push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
        else
        {
            out->push_back(static_cast<char>(0xf0 | (code >> 18)));
            out->push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
            out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out->push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
    }

    bool hex4(unsigned *out)
    {
        if (at_ + 4 > s_.size())
            return false;
        unsigned v = 0;
        for (int i = 0; i < 4; ++i)
        {
            const char c = s_[at_++];
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= static_cast<unsigned>(c - 'A' + 10);
            else
                return false;
        }
        *out = v;
        return true;
    }

    bool string(std::string *out)
    {
        ++at_; // opening quote
        while (at_ < s_.size())
        {
            const char c = s_[at_++];
            if (c == '"')
                return true;
            if (c != '\\')
            {
                out->push_back(c);
                continue;
            }
            if (at_ >= s_.size())
                return false;
            const char e = s_[at_++];
            switch (e)
            {
            case 'n':
                out->push_back('\n');
                break;
            case 't':
                out->push_back('\t');
                break;
            case 'r':
            case 'b':
            case 'f':
                break;
            case 'u':
            {
                unsigned code = 0;
                if (!hex4(&code))
                    return false;
                if (code >= 0xd800 && code < 0xdc00 && at_ + 1 < s_.size() && s_[at_] == '\\' &&
                    s_[at_ + 1] == 'u')
                {
                    at_ += 2;
                    unsigned low = 0;
                    if (!hex4(&low))
                        return false;
                    code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                }
                utf8(out, code);
                break;
            }
            default:
                out->push_back(e); // \" \\ \/
            }
        }
        return false;
    }

    std::string_view s_;
    std::size_t at_ = 0;
};

const Json *member(const Json &j, const std::string &key)
{
    const auto it = j.keys.find(key);
    return it == j.keys.end() ? nullptr : it->second.get();
}

std::string string_member(const Json &j, const std::string &key)
{
    const Json *m = member(j, key);
    return m != nullptr && m->is_string ? m->text : std::string();
}

} // namespace

std::string title_name_from_param_json(std::string_view json)
{
    Json root;
    Parser parser(json);
    if (!parser.value(&root) || !root.is_object)
        return {};
    if (const Json *loc = member(root, "localizedParameters"); loc != nullptr && loc->is_object)
    {
        const std::string language = string_member(*loc, "defaultLanguage");
        for (const std::string &lang : {language, std::string("en-US"), std::string("en-GB")})
            if (const Json *l = member(*loc, lang); l != nullptr && l->is_object)
                if (std::string name = string_member(*l, "titleName"); !name.empty())
                    return name;
        for (const auto &[key, value] : loc->keys)
            if (value->is_object)
                if (std::string name = string_member(*value, "titleName"); !name.empty())
                    return name;
    }
    return string_member(root, "titleName");
}

namespace
{

std::string column_text(sqlite3_stmt *query, int column)
{
    const unsigned char *text = sqlite3_column_text(query, column);
    return text != nullptr ? std::string(reinterpret_cast<const char *>(text)) : std::string();
}

// The first row's text columns for `sql`, with ?1 bound to key; empty when
// the statement does not prepare (no such table or column) or finds nothing.
std::vector<std::string> first_row(sqlite3 *db, const std::string &sql, const std::string &key,
                                   int columns)
{
    std::vector<std::string> row;
    sqlite3_stmt *query = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &query, nullptr) == sqlite::kOk &&
        sqlite3_bind_text(query, 1, key.c_str(), static_cast<int>(key.size()), nullptr) ==
            sqlite::kOk &&
        sqlite3_step(query) == sqlite::kRow)
        for (int c = 0; c < columns; ++c)
            row.push_back(column_text(query, c));
    sqlite3_finalize(query);
    return row;
}

// Tables whose name starts with prefix (one per console user), quoted as
// identifiers for use in SQL.
std::vector<std::string> tables_named(sqlite3 *db, const std::string &prefix)
{
    std::vector<std::string> tables;
    sqlite3_stmt *list = nullptr;
    if (sqlite3_prepare_v2(db,
                           "SELECT name FROM sqlite_master WHERE type = 'table' AND "
                           "substr(name, 1, length(?1)) = ?1 ORDER BY name",
                           -1, &list, nullptr) == sqlite::kOk &&
        sqlite3_bind_text(list, 1, prefix.c_str(), static_cast<int>(prefix.size()), nullptr) ==
            sqlite::kOk)
        while (sqlite3_step(list) == sqlite::kRow)
        {
            std::string quoted = "\"";
            for (const char c : column_text(list, 0))
                quoted += c == '"' ? std::string("\"\"") : std::string(1, c);
            tables.push_back(quoted + "\"");
        }
    sqlite3_finalize(list);
    return tables;
}

// Only a file path is of use: drop the cache-busting "?ts=..." and anything
// that is not absolute (a URL, say). Repeats are skipped.
void add_icon_path(std::string path, std::vector<std::string> *paths)
{
    path = path.substr(0, path.find('?'));
    if (!path.empty() && path.front() == '/' &&
        std::find(paths->begin(), paths->end(), path) == paths->end())
        paths->push_back(std::move(path));
}

} // namespace

AppDbTitle title_from_app_db(const std::string &path, std::string_view id)
{
    AppDbTitle found;
    sqlite3 *db = nullptr;
    const std::string uri = "file:" + path + "?immutable=1";
    if (sqlite3_open_v2(uri.c_str(), &db, sqlite::kOpenReadOnly | sqlite::kOpenUri, nullptr) !=
        sqlite::kOk)
    {
        sqlite3_close(db);
        return {};
    }
    const std::string key(id);

    // PS5 layout. A database without the icon column still names the title.
    std::vector<std::string> row = first_row(
        db, "SELECT titleName, icon0Info FROM tbl_contentinfo WHERE titleId = ?1 LIMIT 1", key, 2);
    if (row.empty())
        row = first_row(db, "SELECT titleName FROM tbl_contentinfo WHERE titleId = ?1 LIMIT 1", key,
                        1);
    if (!row.empty())
        found.name = row[0];
    if (row.size() > 1)
        add_icon_path(row[1], &found.icon_paths);

    // The game's concept (its store page) has an icon of its own, kept where
    // the app can read it even when the title's icon is not.
    for (const std::string &table : tables_named(db, "tbl_concepticoninfo_"))
        if (const std::vector<std::string> icon = first_row(
                db, "SELECT icon0Info FROM " + table + " WHERE primaryTitleId = ?1 LIMIT 1", key,
                1);
            !icon.empty())
            add_icon_path(icon[0], &found.icon_paths);

    // Older layout: the name in tbl_appbrowse_* tables.
    if (found.name.empty())
        for (const std::string &table : tables_named(db, "tbl_appbrowse_"))
            if (const std::vector<std::string> name = first_row(
                    db, "SELECT titleName FROM " + table + " WHERE titleId = ?1 LIMIT 1", key, 1);
                !name.empty() && !name[0].empty())
            {
                found.name = name[0];
                break;
            }
    sqlite3_close(db);
    return found;
}

std::string title_name_from_app_db(const std::string &path, std::string_view id)
{
    return title_from_app_db(path, id).name;
}

TitleLoader::TitleLoader(const FileSystem &fs, std::vector<std::string> roots, int icon_size,
                         std::string app_db)
    : fs_(fs), roots_(std::move(roots)), icon_size_(icon_size), app_db_(std::move(app_db)),
      thread_([this] { run(); })
{
}

TitleLoader::~TitleLoader()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void TitleLoader::request(std::string_view id)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::find(requested_.begin(), requested_.end(), id) != requested_.end())
            return;
        requested_.emplace_back(id);
        queue_.emplace_back(id);
    }
    cv_.notify_all();
}

std::vector<TitleInfo> TitleLoader::poll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TitleInfo> out = std::move(done_);
    done_.clear();
    return out;
}

void TitleLoader::wait_idle()
{
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return queue_.empty() && !working_; });
}

void TitleLoader::run()
{
    for (;;)
    {
        std::string id;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_)
                return;
            id = std::move(queue_.front());
            queue_.pop_front();
            working_ = true;
        }
        TitleInfo info = load(id);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_.push_back(std::move(info));
            working_ = false;
        }
        cv_.notify_all();
    }
}

TitleInfo TitleLoader::load(const std::string &id) const
{
    TitleInfo info;
    info.id = id;
    // The name and the icon are looked for separately: a folder may give one
    // and not the other.
    for (const std::string &root : roots_)
    {
        const std::string folder = root + "/" + id;
        std::string bytes;
        if (info.name.empty() && fs_.read_file(folder + "/param.json", &bytes, 1u << 20))
            info.name = title_name_from_param_json(bytes);
        std::string icon;
        if (info.icon.rgba.empty() && fs_.read_file(folder + "/icon0.png", &icon, 8u << 20))
            decode_image(icon, icon_size_, &info.icon);
        if (!info.name.empty() && !info.icon.rgba.empty())
            break;
    }
    if ((info.name.empty() || info.icon.rgba.empty()) && !app_db_.empty())
    {
        const AppDbTitle db = title_from_app_db(app_db_, id);
        if (info.name.empty())
            info.name = db.name;
        for (const std::string &icon_path : db.icon_paths)
        {
            std::string icon;
            if (!info.icon.rgba.empty())
                break;
            if (fs_.read_file(icon_path, &icon, 8u << 20))
                decode_image(icon, icon_size_, &info.icon);
        }
    }
    return info;
}

} // namespace hui::disk
