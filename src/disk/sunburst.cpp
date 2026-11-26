// Prospero Disk - Sunburst layout: rings of segments, one ring per folder depth.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "disk/sunburst.hpp"

#include <algorithm>
#include <cctype>

namespace hui::disk
{

namespace
{

constexpr float kTau = 6.2831853f;

bool name_less(std::string_view a, std::string_view b)
{
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i)
    {
        const int ca = std::tolower(static_cast<unsigned char>(a[i]));
        const int cb = std::tolower(static_cast<unsigned char>(b[i]));
        if (ca != cb)
            return ca < cb;
    }
    return a.size() < b.size();
}

struct Pending
{
    std::uint32_t node;
    int branch;
    float start;
    float sweep;
};

} // namespace

std::vector<std::uint32_t> sorted_children(const Tree &tree, std::uint32_t index, bool by_name)
{
    std::vector<std::uint32_t> out = tree.children(index);
    std::sort(out.begin(), out.end(),
              [&](std::uint32_t a, std::uint32_t b)
              {
                  if (!by_name && tree.node(a).disk != tree.node(b).disk)
                      return tree.node(a).disk > tree.node(b).disk;
                  return name_less(tree.name(a), tree.name(b));
              });
    return out;
}

std::vector<Segment> layout_sunburst(const Tree &tree, std::uint32_t centre,
                                     const SunburstOptions &options)
{
    std::vector<Segment> out;
    const std::uint64_t whole = tree.node(centre).disk + options.hidden;
    if (whole == 0)
        return out;

    // Breadth first, so each ring is contiguous in the result.
    // The centre belongs to no branch: its "smaller" group on the first ring
    // must not light up with branch 0 when that branch has the focus.
    std::vector<Pending> level{{centre, -1, 0.0f, kTau}};
    for (int ring = 1; ring <= options.rings && !level.empty(); ++ring)
    {
        std::vector<Pending> next;
        int branch_counter = 0;
        for (const Pending &p : level)
        {
            const std::uint64_t total = p.node == centre ? whole : tree.node(p.node).disk;
            if (total == 0)
                continue;
            const double per_byte = static_cast<double>(p.sweep) / static_cast<double>(total);
            float at = p.start;
            std::uint64_t small_bytes = 0;
            for (const std::uint32_t child : sorted_children(tree, p.node, false))
            {
                const std::uint64_t bytes = tree.node(child).disk;
                if (bytes == 0)
                    continue;
                const auto sweep = static_cast<float>(static_cast<double>(bytes) * per_byte);
                if (sweep < options.min_sweep)
                {
                    small_bytes += bytes;
                    continue;
                }
                const int branch = ring == 1 ? branch_counter++ : p.branch;
                out.push_back({SegmentKind::node, child, ring, branch, at, sweep, bytes});
                if (tree.node(child).kind == Kind::directory)
                    next.push_back({child, branch, at, sweep});
                at += sweep;
            }
            const auto small_sweep =
                static_cast<float>(static_cast<double>(small_bytes) * per_byte);
            // A group too thin to see would only draw a hairline.
            if (small_bytes != 0 && small_sweep >= options.min_sweep * 0.5f)
            {
                const float sweep = small_sweep;
                out.push_back(
                    {SegmentKind::smaller, p.node, ring, p.branch, at, sweep, small_bytes});
                at += sweep;
            }
            if (ring == 1 && options.hidden != 0)
            {
                const auto sweep =
                    static_cast<float>(static_cast<double>(options.hidden) * per_byte);
                out.push_back({SegmentKind::hidden, kNoNode, ring, -1, at, sweep, options.hidden});
            }
        }
        level = std::move(next);
    }
    return out;
}

int segment_at(const std::vector<Segment> &segments, float angle)
{
    while (angle < 0.0f)
        angle += kTau;
    while (angle >= kTau)
        angle -= kTau;
    for (std::size_t i = 0; i < segments.size() && segments[i].ring == 1; ++i)
        if (angle >= segments[i].start && angle < segments[i].start + segments[i].sweep)
            return static_cast<int>(i);
    return -1;
}

} // namespace hui::disk
