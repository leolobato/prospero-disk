// Prospero Disk - Numbers as people read them: sizes, counts, durations and ages.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>

namespace hui::disk
{

// Decimal units, as the console's own storage screen counts: "48.2 GB".
std::string format_bytes(std::uint64_t bytes);
// "12,480".
std::string format_count(std::uint64_t count);
// "4.2 s", "1 min 12 s".
std::string format_duration(double seconds);
// "just now", "5 min ago", "2 h ago", "3 days ago".
std::string format_age(std::int64_t seconds);
// "2026-11-17 14:05" in UTC.
std::string format_date(std::int64_t epoch_seconds);
// "18%" (or "<1%").
std::string format_share(std::uint64_t part, std::uint64_t whole);

} // namespace hui::disk
