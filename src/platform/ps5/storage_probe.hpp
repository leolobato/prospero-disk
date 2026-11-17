// Prospero Disk - The M1 storage probe: what the elevation made readable (docs/PRD.md 10.1-3).
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <string>

namespace hui::ps5
{

// A plain-text report: every mount (getfsstat), whether each candidate
// volume and metadata path can be listed and statfs'd, and st_blocks against
// st_size for one installed file. Read-only; takes well under a second.
// Each line goes to emit as soon as it is known, so that a call the sandbox
// refuses by killing the process still leaves the lines before it in the log.
// getfsstat runs last for that reason.
std::string probe_storage(const std::function<void(const std::string &)> &emit);

} // namespace hui::ps5
