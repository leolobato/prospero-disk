// Prospero Disk - Behaviour of collect and delete in the app, on the fixture volumes.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concept_fixture.hpp"
#include "concepts/concepts.hpp"

#include <chrono>
#include <set>
#include <thread>

namespace
{

using hui::Action;
using hui::action_bit;
using hui::Direction;
using hui::InputFrame;
using hui::audio::Cue;
using hui::testing::ConceptFixture;

class DiskCollect : public ConceptFixture
{
  protected:
    void SetUp() override
    {
        design_ = hui::concepts::make_disk(context_);
        design_->enter();
        send(*design_, idle(), 0.5f);
        // Scan the fixture's Internal volume (paced: about three seconds).
        send(*design_, press(Action::confirm), 4.0f);
        settle(1.0f);
    }

    // The fixture's scan and deletion are paced by the screen's clock, but
    // their worker threads also need real time to use what it allows.
    std::set<Cue> settle(float seconds)
    {
        std::set<Cue> cues;
        for (int i = 0, steps = static_cast<int>(seconds * 40.0f); i < steps; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            for (int frame = 0; frame < 6; ++frame)
            {
                feedback_.clear();
                design_->update(idle(), 1.0f / 60.0f, feedback_);
                for (const auto &event : feedback_.cues)
                    cues.insert(event.cue);
            }
        }
        return cues;
    }

    // Sends input for `seconds`, the first frame pressing and every frame
    // holding `held`; returns every cue asked for meanwhile.
    std::set<Cue> hold(std::uint32_t held, float seconds)
    {
        std::set<Cue> cues;
        const int frames = static_cast<int>(seconds * 60.0f);
        for (int frame = 0; frame < frames; ++frame)
        {
            InputFrame input = idle();
            input.held = held;
            input.pressed = frame == 0 ? held : 0u;
            feedback_.clear();
            design_->update(input, 1.0f / 60.0f, feedback_);
            for (const auto &event : feedback_.cues)
                cues.insert(event.cue);
        }
        return cues;
    }

    void down(int times)
    {
        for (int i = 0; i < times; ++i)
            send(*design_, nav(Direction::down), 0.1f);
    }

    // At the fixture's root: app, System & hidden, patch, home, download.
    void collect_download()
    {
        down(4);
        send(*design_, press(Action::jump_next), 0.2f);
        ASSERT_TRUE(played(Cue::favorite_on));
    }

    // From Explore: R3, down past the items to Delete, Cross, then Continue.
    void open_hold()
    {
        send(*design_, press(Action::r3), 0.6f);
        ASSERT_TRUE(played(Cue::modal_open));
        down(3);
        send(*design_, press(Action::confirm), 0.5f); // Delete...
        ASSERT_TRUE(played(Cue::modal_open));
        send(*design_, nav(Direction::right), 0.2f);
        send(*design_, press(Action::confirm), 0.5f); // Continue
        ASSERT_TRUE(played(Cue::modal_open));
    }

    std::unique_ptr<hui::app::Concept> design_;
};

TEST_F(DiskCollect, R2CollectsPutsBackAndRefusesSoftly)
{
    // "app" holds installed games: refused, with the error cue.
    send(*design_, press(Action::jump_next), 0.2f);
    EXPECT_TRUE(played(Cue::error));
    EXPECT_FALSE(played(Cue::favorite_on));
    // "System & hidden" can't be collected either.
    down(1);
    send(*design_, press(Action::jump_next), 0.2f);
    EXPECT_TRUE(played(Cue::error));
    down(3);
    send(*design_, press(Action::jump_next), 0.2f);
    EXPECT_TRUE(played(Cue::favorite_on));
    send(*design_, press(Action::jump_next), 0.2f);
    EXPECT_TRUE(played(Cue::favorite_off));
    expect_drawable(*design_);
}

TEST_F(DiskCollect, ReviewNeedsSomethingCollected)
{
    send(*design_, press(Action::r3), 0.3f);
    EXPECT_TRUE(played(Cue::error));
    EXPECT_FALSE(played(Cue::modal_open));
}

TEST_F(DiskCollect, LettingGoEarlyDeletesNothing)
{
    collect_download();
    open_hold();
    // Held for one second of two, then let go.
    const std::set<Cue> cues = hold(action_bit(Action::confirm), 1.0f);
    EXPECT_EQ(cues.count(Cue::launch), 0u);
    const std::set<Cue> after = hold(0, 0.3f);
    EXPECT_EQ(after.count(Cue::modal_close), 1u);
    EXPECT_EQ(after.count(Cue::launch), 0u);
    // Back in the review, where the item is still collected: Circle closes
    // it and R3 opens it again.
    send(*design_, press(Action::back), 0.6f);
    EXPECT_TRUE(played(Cue::modal_close));
    send(*design_, press(Action::r3), 0.3f);
    EXPECT_TRUE(played(Cue::modal_open));
    expect_drawable(*design_);
}

TEST_F(DiskCollect, CircleOnTheHoldCancels)
{
    collect_download();
    open_hold();
    send(*design_, press(Action::back), 0.3f);
    EXPECT_TRUE(played(Cue::modal_close));
    EXPECT_FALSE(played(Cue::launch));
}

TEST_F(DiskCollect, HoldingTwoSecondsDeletesAndShowsTheResult)
{
    collect_download();
    open_hold();
    const std::set<Cue> cues = hold(action_bit(Action::confirm), 2.3f);
    EXPECT_EQ(cues.count(Cue::launch), 1u); // the deletion started
    expect_drawable(*design_);
    // The fixture deletes in tour time; then the result dialog.
    std::set<Cue> done = settle(2.0f);
    done.insert(cues.begin(), cues.end());
    EXPECT_EQ(done.count(Cue::modal_open), 1u);
    expect_drawable(*design_);
    send(*design_, press(Action::confirm), 0.5f); // OK
    // The collection is empty again.
    send(*design_, press(Action::r3), 0.3f);
    EXPECT_TRUE(played(Cue::error));
}

} // namespace
