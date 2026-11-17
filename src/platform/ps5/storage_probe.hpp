// Prospero Disk - The M1 storage probe: what the elevation made readable (docs/PRD.md 10.1-3).
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace hui::ps5
{

// A plain-text report: every mount (getfsstat), whether each candidate
// volume and metadata path can be listed and statfs'd, and st_blocks against
// st_size for one installed file. Read-only; takes well under a second.
std::string probe_storage();

} // namespace hui::ps5
