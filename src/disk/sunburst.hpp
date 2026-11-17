// Prospero Disk - Sunburst layout: rings of segments, one ring per folder depth.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "disk/tree.hpp"

#include <cstdint>
#include <vector>

namespace hui::disk
{

enum class SegmentKind : std::uint8_t
{
    node,    // one folder or file
    smaller, // the children of one parent that are each too thin to draw
    hidden,  // "System & hidden": used space the scan could not see
};

struct Segment
{
    SegmentKind kind = SegmentKind::node;
    std::uint32_t node = kNoNode; // the node, or the parent of a "smaller" group
    int ring = 1;                 // 1 is the first ring around the centre
    int branch = 0;               // index of its first-ring ancestor, for colour
    float start = 0.0f;           // radians, clockwise from 12 o'clock
    float sweep = 0.0f;
    std::uint64_t bytes = 0;
};

struct SunburstOptions
{
    int rings = 5;
    float min_sweep = 0.0174533f; // about 1 degree (PRD 5.3)
    // Extra bytes shown as a "hidden" segment on the first ring (the root of a
    // volume only); they also count towards the whole circle.
    std::uint64_t hidden = 0;
};

// Lays out the subtree under centre. Segments come ring by ring, and within a
// ring in clockwise order, so the first ring is a prefix of the result.
std::vector<Segment> layout_sunburst(const Tree &tree, std::uint32_t centre,
                                     const SunburstOptions &options);

// A node's children, largest first (ties by name), or by name.
std::vector<std::uint32_t> sorted_children(const Tree &tree, std::uint32_t index, bool by_name);

// The first-ring segment under an angle (radians, clockwise from 12), or -1.
int segment_at(const std::vector<Segment> &segments, float angle);

} // namespace hui::disk
