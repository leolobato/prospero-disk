// Prospero Disk - The write probe: what the elevated app may create and remove, and where.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <string>
#include <sys/types.h>

namespace hui::ps5
{

// Collect and delete (docs/plans/collect-delete.md, section 2) needs to know
// what the app may remove after the Lapy grant. Root has no DAC bypass on this
// console, so a removal depends on who owns the folder an entry is in; the
// probe tries the cases that tell those rules apart in a scratch folder under
// data_root, then creates and removes throwaway entries in the places users
// keep things, as app_user and with the effective uid raised to 0, and lists
// the owners and modes of real entries without changing them. It never
// touches an existing file.
//
// It raises the effective uid, which belongs to the whole process: call it
// only while no other thread runs. A failed restore parks the app (anything
// after it would run as root). Each line goes to emit before the call it
// names, so a call that ends the process still leaves its line in the log.
std::string probe_writes(const std::string &data_root, uid_t app_user,
                         const std::function<void(const std::string &)> &emit);

} // namespace hui::ps5
