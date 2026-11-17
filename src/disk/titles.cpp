// Prospero Disk - Title IDs to names and icons, read off the frame thread (PRD 5.4).
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "disk/titles.hpp"

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

TitleLoader::TitleLoader(const FileSystem &fs, std::vector<std::string> roots, int icon_size)
    : fs_(fs), roots_(std::move(roots)), icon_size_(icon_size), thread_([this] { run(); })
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
    for (const std::string &root : roots_)
    {
        std::string bytes;
        if (!fs_.read_file(root + "/" + id + "/param.json", &bytes, 1u << 20))
            continue;
        info.name = title_name_from_param_json(bytes);
        std::string icon;
        if (fs_.read_file(root + "/" + id + "/icon0.png", &icon, 8u << 20))
            decode_png(icon, icon_size_, &info.icon);
        if (!info.name.empty())
            break;
    }
    return info;
}

} // namespace hui::disk
