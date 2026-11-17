// Prospero Disk - Placeholder start screen until the Volumes screen exists (milestone M3).
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/concepts.hpp"

#include "core/tween.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"

namespace hui::concepts
{

namespace
{

using gfx::Color;
using gfx::Rect;

// The File Browser / Pulse Dashboard language the PRD asks for: dark gradient,
// frosted-looking panels, coloured legend dots.
const Color kInk = Color::rgb(0xf2f4ff);
const Color kMuted = Color::rgb(0x9aa3c7);
const Color kPanel = Color::rgb(0x1b2036, 0.82f);
const Color kAccent = Color::rgb(0x5ec8ff);
constexpr float kMargin = 96.0f;

struct Kind
{
    const char *name;
    const char *where;
    Color dot;
};

// What the scanner will offer (PRD F1); nothing is detected yet.
constexpr int kKinds = 3;
const Kind kKindList[kKinds] = {
    {"Internal", "/user, /data", Color::rgb(0x5ec8ff)},
    {"Extended", "/mnt/ext0, /mnt/ext1", Color::rgb(0xb18cff)},
    {"USB", "/mnt/usb0 - /mnt/usb7", Color::rgb(0xffb35c)},
};

constexpr const char *kTechniques[] = {
    "Placeholder for the Volumes screen (milestone M3)",
    "Rows enter with a stagger; the focus follows a spring",
    "Refuses softly at the list ends",
};

constexpr app::TourStep kTour[] = {
    {0.4f, 0, Direction::down}, {0.4f, 0, Direction::down, "extended"},
    {0.4f, 0, Direction::down}, // refused: the last row
    {0.4f, 0, Direction::up},   {0.4f, 0, Direction::up},
};

class Disk final : public app::Concept
{
  public:
    explicit Disk(app::Context &context) : context_(context)
    {
        highlight_.snap(row_rect(0));
    }

    const app::ConceptInfo &info() const override
    {
        static const app::ConceptInfo kInfo{
            "disk",
            "Prospero Disk",
            "Where your storage went",
            "src/concepts/disk.cpp",
            audio::SoundSet::glass,
            kAccent,
            kTechniques,
        };
        return kInfo;
    }

    void enter() override
    {
        age_ = 0.0f;
    }

    void update(const InputFrame &input, float dt, app::Feedback &feedback) override
    {
        age_ += dt;
        clock_ += dt;
        if (input.nav == Direction::up || input.nav == Direction::down)
        {
            const int next = focus_ + (input.nav == Direction::down ? 1 : -1);
            if (next >= 0 && next < kKinds)
            {
                focus_ = next;
                feedback.play(audio::Cue::focus);
            }
            else if (!input.nav_repeat)
            {
                feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
                refusal_.trigger();
            }
        }
        if (input.is_pressed(Action::confirm))
            feedback.play(audio::Cue::select);

        highlight_.target(row_rect(focus_));
        highlight_.update(dt, 20.0f);
        refusal_.update(dt, 9.0f);
    }

    void draw(app::Frame &frame) const override
    {
        frame.backdrop.mode = gfx::BackdropMode::gradient;
        frame.backdrop.colors[0] = Color::rgb(0x0c0f1c);
        frame.backdrop.colors[1] = Color::rgb(0x1a1634);
        frame.backdrop.time = clock_;

        gfx::DrawList &list = frame.scene;
        const ui::Fonts &fonts = context_.fonts;
        ui::text(list, fonts.display, "Prospero Disk", kMargin, 200, 72, kInk);
        ui::text(list, fonts.regular, "Choose a volume to see what is using its space.", kMargin,
                 256, 30, kMuted);

        Rect ring = highlight_.value();
        ring.x += ui::shake(refusal_.value, clock_);
        list.rounded_rect({ring.x - 4.0f, ring.y - 4.0f, ring.w + 8.0f, ring.h + 8.0f}, 26,
                          kAccent);
        for (int i = 0; i < kKinds; ++i)
        {
            const float in = tween::stagger(age_, i);
            const Rect r = row_rect(i);
            const float lift = 24.0f * (1.0f - in);
            list.push_opacity(in);
            list.rounded_rect({r.x, r.y + lift, r.w, r.h}, 22, kPanel);
            list.circle(r.x + 52, r.y + lift + r.h * 0.5f, 12, kKindList[i].dot);
            ui::text(list, fonts.semibold, kKindList[i].name, r.x + 92, r.y + lift + 58, 36, kInk);
            ui::text(list, fonts.regular, kKindList[i].where, r.x + 92, r.y + lift + 98, 26,
                     kMuted);
            ui::text(list, fonts.regular, "Not scanned", r.x + r.w - 40, r.y + lift + 78, 26,
                     kMuted, gfx::Align::right);
            list.pop_opacity();
        }

        const ui::Hint hints[] = {{ui::Button::cross, "Scan"}};
        ui::draw_hints(list, fonts, ui::GlyphStyle::dark(), hints, 1, 1824, true);
    }

    std::span<const app::TourStep> tour() const override
    {
        return kTour;
    }

  private:
    static Rect row_rect(int index)
    {
        return {kMargin, 330.0f + static_cast<float>(index) * 150.0f, 900, 130};
    }

    app::Context &context_;
    float age_ = 0.0f;
    float clock_ = 0.0f;
    int focus_ = 0;
    ui::SpringRect highlight_;
    ui::Pulse refusal_;
};

} // namespace

std::unique_ptr<app::Concept> make_disk(app::Context &context)
{
    return std::make_unique<Disk>(context);
}

} // namespace hui::concepts
