// Prospero Disk - Numbers as people read them: sizes, counts, durations and ages.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "disk/format.hpp"

#include <cstdio>

namespace hui::disk
{

std::string format_bytes(std::uint64_t bytes)
{
    static constexpr const char *kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1000.0 && unit < 5)
    {
        value /= 1000.0;
        ++unit;
    }
    char text[32];
    if (unit == 0)
        std::snprintf(text, sizeof text, "%llu B", static_cast<unsigned long long>(bytes));
    else if (value < 10.0)
        std::snprintf(text, sizeof text, "%.2f %s", value, kUnits[unit]);
    else if (value < 100.0)
        std::snprintf(text, sizeof text, "%.1f %s", value, kUnits[unit]);
    else
        std::snprintf(text, sizeof text, "%.0f %s", value, kUnits[unit]);
    return text;
}

std::string format_count(std::uint64_t count)
{
    std::string digits = std::to_string(count);
    std::string out;
    const std::size_t n = digits.size();
    for (std::size_t i = 0; i < n; ++i)
    {
        if (i != 0 && (n - i) % 3 == 0)
            out.push_back(',');
        out.push_back(digits[i]);
    }
    return out;
}

std::string format_duration(double seconds)
{
    char text[32];
    if (seconds < 60.0)
        std::snprintf(text, sizeof text, "%.1f s", seconds < 0.0 ? 0.0 : seconds);
    else
    {
        const auto whole = static_cast<long long>(seconds);
        std::snprintf(text, sizeof text, "%lld min %lld s", whole / 60, whole % 60);
    }
    return text;
}

std::string format_age(std::int64_t seconds)
{
    char text[32];
    if (seconds < 60)
        return "just now";
    if (seconds < 3600)
        std::snprintf(text, sizeof text, "%lld min ago", static_cast<long long>(seconds / 60));
    else if (seconds < 86400)
        std::snprintf(text, sizeof text, "%lld h ago", static_cast<long long>(seconds / 3600));
    else
    {
        const long long days = seconds / 86400;
        std::snprintf(text, sizeof text, "%lld day%s ago", days, days == 1 ? "" : "s");
    }
    return text;
}

std::string format_date(std::int64_t epoch_seconds)
{
    // Civil-from-days (Howard Hinnant): no time zone tables on the console.
    const std::int64_t days =
        epoch_seconds >= 0 ? epoch_seconds / 86400 : (epoch_seconds - 86399) / 86400;
    const std::int64_t secs = epoch_seconds - days * 86400;
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const std::int64_t day = doy - (153 * mp + 2) / 5 + 1;
    const std::int64_t month = mp < 10 ? mp + 3 : mp - 9;
    const std::int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
    char text[40];
    std::snprintf(text, sizeof text, "%04lld-%02lld-%02lld %02lld:%02lld",
                  static_cast<long long>(year), static_cast<long long>(month),
                  static_cast<long long>(day), static_cast<long long>(secs / 3600),
                  static_cast<long long>(secs % 3600 / 60));
    return text;
}

std::string format_share(std::uint64_t part, std::uint64_t whole)
{
    if (whole == 0)
        return "0%";
    const double share = 100.0 * static_cast<double>(part) / static_cast<double>(whole);
    if (share > 0.0 && share < 1.0)
        return "<1%";
    char text[16];
    std::snprintf(text, sizeof text, "%.0f%%", share);
    return text;
}

} // namespace hui::disk
