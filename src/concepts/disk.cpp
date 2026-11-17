// Prospero Disk - The app: Volumes, Scanning and Explore (sunburst and list).
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One design owns the whole app (PRD section 5):
//   Volumes   a card per readable volume, its statfs bar and cache age
//   Scanning  live counters and a sunburst that grows as folders finish
//   Explore   the sunburst of the current folder beside its sorted list
// The scanner and the cache live in src/disk; this file only drives them and
// never waits for them: scans run on worker threads, cache files are read
// and written on helper threads, title names and icons on the TitleLoader's.

#include "concepts/concepts.hpp"

#include "core/tween.hpp"
#include "disk/cache.hpp"
#include "disk/format.hpp"
#include "disk/scanner.hpp"
#include "disk/sunburst.hpp"
#include "disk/titles.hpp"
#include "disk/volumes.hpp"
#include "ui/components/dialog.hpp"
#include "ui/components/list.hpp"
#include "ui/components/menu.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"
#include "ui/theme.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <map>
#include <thread>

namespace hui::concepts
{

namespace
{

namespace dk = hui::disk;
using gfx::Color;
using gfx::Rect;

constexpr float kMargin = 96.0f;
constexpr const char *kDot = "  \xc2\xb7  "; // a middle dot between two facts

// The File Browser / Pulse Dashboard language (PRD 7.1).
const Color kInk = Color::rgb(0xf2f4ff);
const Color kMuted = Color::rgb(0x9aa3c7);
const Color kFaint = Color::rgb(0x6b7398);
const Color kAccent = Color::rgb(0x5ec8ff);
const Color kWarn = Color::rgb(0xffb35c);
const Color kSmaller = Color::rgb(0x59607a);
const Color kHiddenA = Color::rgb(0x343a52);
const Color kHiddenB = Color::rgb(0x4a5170);
const Color kCore = Color::rgb(0x151a30, 0.94f);

// One hue per first-ring branch; lightness falls with depth.
constexpr std::uint32_t kBranchColours[] = {0x4f8cff, 0xff6b6b, 0xffb84d, 0x3fd18b,
                                            0xb07cff, 0x2ec5d3, 0xf06fb3, 0x9bd64b};

constexpr const char *kTechniques[] = {
    "A diskus-style parallel scan on worker threads; the UI only try-locks the tree",
    "Sunburst rings from DrawList::arc, slivers merged per parent, hidden space hatched",
    "The list, the menu and the dialogs are kit components in the Acrylic theme",
    "Title names and icons load off the frame thread and fade in",
};

// The tour: Volumes, scan the fixture, explore, re-root, sort, up, menu.
constexpr app::TourStep kTour[] = {
    {0.6f, 0, Direction::none, "start"},
    {0.1f, action_bit(Action::confirm)}, // scan Internal
    {1.0f, 0, Direction::none, "scanning"},
    {2.6f, 0, Direction::none, "explore"},
    {0.4f, 0, Direction::down},
    {0.6f, 0, Direction::down, "selected"},
    {0.3f, 0, Direction::up},
    {0.3f, 0, Direction::up},
    {0.5f, action_bit(Action::confirm)}, // into the largest folder
    {0.9f, 0, Direction::none, "folder"},
    {0.12f, 0, Direction::down},
    {0.12f, 0, Direction::down},
    {0.12f, 0, Direction::down},
    {0.12f, 0, Direction::down},
    {0.12f, 0, Direction::down},
    {0.12f, 0, Direction::down},
    {0.12f, 0, Direction::down},
    {0.12f, 0, Direction::down},
    {0.12f, 0, Direction::down},
    {0.12f, 0, Direction::down},
    {0.12f, 0, Direction::down},
    {0.8f, 0, Direction::none, "scripts"},
    {0.08f, 0, Direction::up},
    {0.08f, 0, Direction::up},
    {0.08f, 0, Direction::up},
    {0.08f, 0, Direction::up},
    {0.08f, 0, Direction::up},
    {0.08f, 0, Direction::up},
    {0.08f, 0, Direction::up},
    {0.08f, 0, Direction::up},
    {0.08f, 0, Direction::up},
    {0.08f, 0, Direction::up},
    {0.08f, 0, Direction::up},
    {0.3f, 0, Direction::none},
    {0.5f, action_bit(Action::confirm)}, // and the largest one in it
    {0.9f, 0, Direction::none, "titles"},
    {0.3f, action_bit(Action::north)}, // sort by name
    {0.7f, 0, Direction::none, "by-name"},
    {0.3f, action_bit(Action::north)}, // by size again
    {0.4f, action_bit(Action::back)},
    {0.5f, action_bit(Action::back)}, // up to the root
    {0.6f, action_bit(Action::menu)},
    {0.5f, 0, Direction::down, "menu"},
    {0.3f, action_bit(Action::confirm)}, // Scan details
    {0.7f, 0, Direction::none, "details"},
    {0.3f, action_bit(Action::confirm)}, // OK
    {0.6f, action_bit(Action::back)},    // to Volumes
    {0.8f, 0, Direction::none, "volumes"},
};

const ui::Theme &app_theme()
{
    for (const ui::Theme &t : ui::themes())
        if (std::string_view(t.id) == "acrylic")
            return t;
    return ui::themes()[0];
}

Color branch_colour(int branch, int ring)
{
    if (branch < 0)
        return kSmaller;
    const Color base = Color::rgb(kBranchColours[static_cast<std::size_t>(branch) % 8]);
    return gfx::mix(base, Color::rgb(0x141830), 0.13f * static_cast<float>(std::max(0, ring - 1)));
}

const char *kind_label(dk::VolumeKind kind)
{
    switch (kind)
    {
    case dk::VolumeKind::internal:
        return "Internal";
    case dk::VolumeKind::extended:
        return "M.2";
    case dk::VolumeKind::usb:
        return "USB";
    case dk::VolumeKind::folder:
        return "Folder";
    }
    return "";
}

std::int64_t wall_seconds()
{
    return static_cast<std::int64_t>(std::time(nullptr));
}

// A row of the list, copied out of the tree so drawing never touches it.
struct Row
{
    dk::SegmentKind kind = dk::SegmentKind::node;
    std::uint32_t node = dk::kNoNode;
    dk::Kind node_kind = dk::Kind::directory;
    std::uint8_t flags = 0;
    std::string name;
    std::string title_id; // set when a folder is named after a title
    std::uint64_t disk = 0;
    std::uint64_t apparent = 0;
    std::uint64_t files = 0;
    std::uint32_t folders = 0;
    std::int64_t mtime = 0;
    int branch = -1; // colour; -1 for grey
};

// What the screen shows of one folder: its rings and its rows.
struct View
{
    std::uint32_t centre = 0;
    std::string centre_name;
    std::string path;
    std::uint64_t centre_disk = 0;
    std::uint64_t hidden = 0;
    std::vector<dk::Segment> segments;
    std::vector<Row> rows;
};

View build_view(const dk::Tree &tree, std::uint32_t centre, bool by_name, std::uint64_t hidden)
{
    View v;
    v.centre = centre;
    v.centre_name = std::string(tree.name(centre));
    v.path = tree.path(centre);
    v.centre_disk = tree.node(centre).disk;
    v.hidden = hidden;
    dk::SunburstOptions options;
    options.hidden = hidden;
    v.segments = dk::layout_sunburst(tree, centre, options);

    std::map<std::uint32_t, int> branches;
    for (const dk::Segment &s : v.segments)
        if (s.ring == 1 && s.kind == dk::SegmentKind::node)
            branches[s.node] = s.branch;
    for (const std::uint32_t c : dk::sorted_children(tree, centre, by_name))
    {
        const dk::Node &n = tree.node(c);
        Row r;
        r.node = c;
        r.node_kind = n.kind;
        r.flags = n.flags;
        r.name = std::string(tree.name(c));
        if (n.kind == dk::Kind::directory && dk::is_title_id(r.name))
            r.title_id = r.name;
        r.disk = n.disk;
        r.apparent = n.apparent;
        r.files = n.files;
        r.folders = n.folders;
        r.mtime = n.mtime;
        const auto it = branches.find(c);
        r.branch = it == branches.end() ? -1 : it->second;
        v.rows.push_back(std::move(r));
    }
    if (hidden != 0)
    {
        Row r;
        r.kind = dk::SegmentKind::hidden;
        r.name = "System & hidden";
        r.disk = hidden;
        auto at = v.rows.end();
        if (!by_name) // largest first: it goes where its size puts it
        {
            at = v.rows.begin();
            while (at != v.rows.end() && at->disk >= hidden)
                ++at;
        }
        v.rows.insert(at, std::move(r));
    }
    return v;
}

// Loads one cache file on a helper thread.
struct LoadJob
{
    std::size_t volume = 0;
    std::thread thread;
    std::atomic<bool> done{false};
    bool ok = false;
    dk::CachedScan scan;
};

// Writes one cache file on a helper thread.
struct SaveJob
{
    std::thread thread;
    std::atomic<bool> done{false};
    std::string error;
};

struct VolumeState
{
    dk::Volume volume;
    bool cached = false; // a cache file exists for it
    dk::CacheInfo info;  // of the cache file, or of the scan in memory
    std::unique_ptr<dk::Tree> tree;
    bool partial = false; // a cancelled scan: shown, never cached
    std::vector<std::string> unreadable;
};

struct TitleState
{
    std::string name;
    std::uint32_t texture = 0;
    float age = 0.0f; // since the icon arrived, for the fade
};

enum class Screen : std::uint8_t
{
    volumes,
    loading,
    scanning,
    explore,
};

enum MenuTag : int
{
    kMenuRescan = 1,
    kMenuDetails = 2,
    kMenuAbout = 3,
};

ui::MenuItem menu_item(const char *label, const char *shortcut, int tag)
{
    ui::MenuItem item;
    item.label = label;
    item.shortcut = shortcut;
    item.tag = tag;
    return item;
}

class Disk final : public app::Concept
{
  public:
    explicit Disk(app::Context &context)
        : context_(context), env_(dk::make_environment()),
          titles_(*env_.fs, env_.appmeta_roots, 128, env_.app_db)
    {
        const ui::Theme &theme = app_theme();
        volume_list_.style.theme = theme;
        volume_list_.style.row_height = 150.0f;
        volume_list_.style.gap = 18.0f;
        volume_list_.style.cards = true;
        volume_list_.style.entrance_step = 0.05f;
        volume_list_.content = [this](ui::Canvas &canvas, const Rect &row, const ui::ListItem &item,
                                      int, float) { draw_volume_card(canvas, row, item); };

        rows_.style.theme = theme;
        rows_.style.row_height = 70.0f;
        rows_.style.gap = 4.0f;
        rows_.style.panel = true;
        rows_.style.entrance_step = 0.02f;
        rows_.content = [this](ui::Canvas &canvas, const Rect &row, const ui::ListItem &item, int,
                               float focus) { draw_row(canvas, row, item, focus); };

        menu_.style.theme = theme;
        menu_.style.scrim = 0.35f;
        menu_.set_items({menu_item("Rescan", "Square", kMenuRescan),
                         menu_item("Scan details", "", kMenuDetails),
                         menu_item("About", "", kMenuAbout)});
        dialog_.style.theme = theme;

        for (const dk::Volume &v : dk::detect_volumes(*env_.fs, env_.specs))
        {
            VolumeState state;
            state.volume = v;
            if (!env_.cache_dir.empty())
                state.cached =
                    dk::read_cache_info(dk::cache_path(env_.cache_dir, v.spec.key), &state.info);
            volumes_.push_back(std::move(state));
        }
        refresh_volume_list();
        burst_.snap(1.0f);
    }

    ~Disk() override
    {
        scanner_.cancel();
        scanner_.join();
        if (load_ && load_->thread.joinable())
            load_->thread.join();
        if (save_ && save_->thread.joinable())
            save_->thread.join();
    }

    Disk(const Disk &) = delete;
    Disk &operator=(const Disk &) = delete;

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
        volume_list_.enter();
    }

    std::span<const app::TourStep> tour() const override
    {
        return kTour;
    }

    void update(const InputFrame &input, float dt, app::Feedback &feedback) override
    {
        clock_ += dt;
        env_.fs->advance(static_cast<double>(dt));
        poll_jobs(feedback);
        poll_titles();

        if (dialog_.is_open())
            dialog_.handle(input, feedback);
        else if (menu_.is_open())
            handle_menu(input, feedback);
        else
            switch (screen_)
            {
            case Screen::volumes:
                handle_volumes(input, feedback);
                break;
            case Screen::loading:
                break;
            case Screen::scanning:
                handle_scanning(input, feedback, dt);
                break;
            case Screen::explore:
                handle_explore(input, feedback);
                break;
            }

        volume_list_.update(dt);
        rows_.update(dt);
        menu_.update(dt);
        dialog_.update(dt);
        burst_.update(dt, context_.settings.reduced_motion ? 40.0f : 11.0f);
        refusal_.update(dt, 9.0f);
        for (auto &entry : titles_by_id_)
            entry.second.age += dt;
    }

    void draw(app::Frame &frame) const override
    {
        frame.backdrop.mode = gfx::BackdropMode::gradient;
        frame.backdrop.colors[0] = Color::rgb(0x0b0e1a);
        frame.backdrop.colors[1] = Color::rgb(0x191533);
        frame.backdrop.time = clock_;
        frame.glass = menu_.visible() || dialog_.is_open();

        ui::Canvas canvas{frame.scene, context_.fonts, 0, clock_};
        switch (screen_)
        {
        case Screen::volumes:
            draw_volumes(canvas);
            break;
        case Screen::loading:
            draw_loading(canvas);
            break;
        case Screen::scanning:
            draw_scanning(canvas);
            break;
        case Screen::explore:
            draw_explore(canvas);
            break;
        }

        ui::Canvas overlay{frame.overlay, context_.fonts, frame.glass_texture, clock_};
        menu_.draw(overlay);
        dialog_.draw(overlay);
    }

  private:
    // ---------------------------------------------------------------- jobs

    void poll_jobs(app::Feedback &feedback)
    {
        if (load_ && load_->done)
        {
            load_->thread.join();
            const std::size_t index = load_->volume;
            VolumeState &v = volumes_[index];
            if (load_->ok)
            {
                v.tree = std::move(load_->scan.tree);
                v.info = load_->scan.info;
                v.partial = false;
                load_.reset();
                open_explore(index);
            }
            else
            {
                // A corrupt or old cache is dropped silently (PRD 7.4).
                v.cached = false;
                load_.reset();
                start_scan(index, feedback);
            }
        }
        if (save_ && save_->done)
        {
            save_->thread.join();
            if (!save_->error.empty())
                last_error_ = save_->error;
            save_.reset();
        }
        if (screen_ == Screen::scanning && scanner_.progress().finished)
            finish_scan(feedback);
    }

    void poll_titles()
    {
        for (dk::TitleInfo &t : titles_.poll())
        {
            TitleState &state = titles_by_id_[t.id];
            state.name = t.name;
            if (!t.icon.rgba.empty() && context_.create_texture)
            {
                state.texture =
                    context_.create_texture(t.icon.width, t.icon.height, t.icon.rgba.data());
                state.age = 0.0f;
            }
        }
    }

    // ------------------------------------------------------------ volumes

    void refresh_volume_list()
    {
        std::vector<ui::ListItem> items;
        for (std::size_t i = 0; i < volumes_.size(); ++i)
        {
            ui::ListItem item;
            item.title = volumes_[i].volume.spec.name;
            item.tag = static_cast<int>(i);
            items.push_back(item);
        }
        const int focus = volume_list_.focus();
        volume_list_.set_items(std::move(items));
        if (!volumes_.empty())
            volume_list_.set_focus(std::clamp(focus, 0, static_cast<int>(volumes_.size()) - 1));
        const float top = degraded() ? 320.0f : 290.0f;
        volume_list_.set_bounds({kMargin, top, 1100.0f, 940.0f - top});
    }

    bool degraded() const
    {
        return env_.elevation.attempted && !env_.elevation.granted;
    }

    void handle_volumes(const InputFrame &input, app::Feedback &feedback)
    {
        if (input.is_pressed(Action::menu))
        {
            show_about(feedback);
            return;
        }
        if (volumes_.empty())
            return;
        const ui::Event event = volume_list_.handle(input, feedback);
        const auto index = static_cast<std::size_t>(volume_list_.focus());
        if (event == ui::Event::activated)
            open_volume(index, feedback);
        else if (input.is_pressed(Action::west))
            start_scan(index, feedback);
    }

    // Cross on a card: what is in memory, else the cache, else a scan.
    void open_volume(std::size_t index, app::Feedback &feedback)
    {
        VolumeState &v = volumes_[index];
        if (v.tree)
        {
            open_explore(index);
            return;
        }
        if (v.cached && !load_)
        {
            load_ = std::make_unique<LoadJob>();
            load_->volume = index;
            const std::string path = dk::cache_path(env_.cache_dir, v.volume.spec.key);
            LoadJob *job = load_.get();
            job->thread = std::thread(
                [job, path]
                {
                    job->ok = dk::load_cache(path, &job->scan);
                    job->done = true;
                });
            screen_ = Screen::loading;
            loading_volume_ = index;
            return;
        }
        start_scan(index, feedback);
    }

    // ----------------------------------------------------------- scanning

    void start_scan(std::size_t index, app::Feedback &feedback)
    {
        const VolumeState &v = volumes_[index];
        dk::ScanOptions options;
        options.root = v.volume.spec.path;
        options.threads = dk::default_threads(v.volume.spec.kind);
        if (!scanner_.start(*env_.fs, options))
        {
            feedback.play(audio::Cue::error);
            refusal_.trigger();
            return;
        }
        scanning_volume_ = index;
        scanning_view_ = View{};
        scan_refresh_ = 0.0f;
        scan_age_ = 0.0f;
        screen_ = Screen::scanning;
        feedback.play(audio::Cue::launch);
    }

    void handle_scanning(const InputFrame &input, app::Feedback &feedback, float dt)
    {
        scan_age_ += dt;
        scan_refresh_ -= dt;
        if (scan_refresh_ <= 0.0f)
        {
            // Re-read the growing tree a few times a second, and only if no
            // worker holds it right now.
            if (scanner_.try_read([this](const dk::Tree &tree)
                                  { scanning_view_ = build_view(tree, tree.root(), false, 0); }))
                scan_refresh_ = 0.25f;
        }
        if (!input.is_pressed(Action::back))
            return;
        // A cancelled scan is shown as partial and never cached (PRD 5.2).
        scanner_.cancel();
        scanner_.join();
        VolumeState &v = volumes_[scanning_volume_];
        std::unique_ptr<dk::Tree> partial;
        scanner_.read([&partial](const dk::Tree &tree)
                      { partial = std::make_unique<dk::Tree>(tree); });
        feedback.play(audio::Cue::back);
        if (partial && partial->node(partial->root()).disk != 0)
        {
            v.tree = std::move(partial);
            v.partial = true;
            v.unreadable = scanner_.unreadable_paths();
            v.info = scan_info(v);
            open_explore(scanning_volume_);
        }
        else
            back_to_volumes(scanning_volume_);
    }

    dk::CacheInfo scan_info(const VolumeState &v) const
    {
        dk::CacheInfo info;
        info.volume_key = v.volume.spec.key;
        info.space = v.volume.space;
        info.scanned_at = wall_seconds();
        info.duration_ms = static_cast<std::uint64_t>(scanner_.duration() * 1000.0);
        info.unreadable = v.unreadable.size();
        return info;
    }

    void finish_scan(app::Feedback &feedback)
    {
        VolumeState &v = volumes_[scanning_volume_];
        // statfs again: the numbers should add up with what was just counted.
        dk::SpaceInfo space;
        if (env_.fs->space(v.volume.spec.path, &space))
        {
            v.volume.space = space;
            v.volume.has_space = true;
        }
        v.unreadable = scanner_.unreadable_paths();
        v.info = scan_info(v);
        v.tree = scanner_.take();
        v.partial = false;
        if (!v.tree)
        {
            back_to_volumes(scanning_volume_);
            return;
        }
        if (!env_.cache_dir.empty() && !save_)
        {
            // A copy, so the helper never shares the tree with the screen.
            save_ = std::make_unique<SaveJob>();
            SaveJob *job = save_.get();
            auto copy = std::make_shared<dk::Tree>(*v.tree);
            const dk::CacheInfo info = v.info;
            const std::string dir = env_.cache_dir;
            job->thread = std::thread(
                [job, copy, info, dir]
                {
                    job->error = dk::save_cache(dir, info, *copy);
                    job->done = true;
                });
            v.cached = true;
        }
        feedback.play(audio::Cue::notify);
        open_explore(scanning_volume_);
    }

    void back_to_volumes(std::size_t focus)
    {
        screen_ = Screen::volumes;
        refresh_volume_list();
        volume_list_.set_focus(static_cast<int>(focus));
        volume_list_.enter();
    }

    // ------------------------------------------------------------ explore

    std::uint64_t hidden_bytes(const VolumeState &v, std::uint32_t centre) const
    {
        // Only at the volume root: what statfs calls used and the scan could
        // not see, so that the numbers add up (PRD 5.3).
        if (centre != v.tree->root() || !v.volume.has_space || v.partial)
            return 0;
        const std::uint64_t seen = v.tree->node(v.tree->root()).disk;
        return v.volume.space.used > seen ? v.volume.space.used - seen : 0;
    }

    void open_explore(std::size_t index)
    {
        current_volume_ = index;
        screen_ = Screen::explore;
        set_centre(volumes_[index].tree->root(), 0.82f, dk::kNoNode);
    }

    // Re-roots the picture. focus_node stays focused (going up); burst_from
    // is where the rings' scale springs from: below 1 grows in, above 1 settles.
    void set_centre(std::uint32_t centre, float burst_from, std::uint32_t focus_node)
    {
        const VolumeState &v = volumes_[current_volume_];
        view_ = build_view(*v.tree, centre, by_name_, hidden_bytes(v, centre));
        std::vector<ui::ListItem> items;
        int focus = 0;
        for (std::size_t i = 0; i < view_.rows.size(); ++i)
        {
            ui::ListItem item;
            item.title = view_.rows[i].name;
            item.tag = static_cast<int>(i);
            items.push_back(item);
            if (focus_node != dk::kNoNode && view_.rows[i].node == focus_node)
                focus = static_cast<int>(i);
            if (!view_.rows[i].title_id.empty())
                titles_.request(view_.rows[i].title_id);
        }
        rows_.set_items(std::move(items));
        rows_.set_bounds({1010.0f, 196.0f, 814.0f, 552.0f});
        rows_.set_focus(focus);
        rows_.enter();
        burst_.snap(context_.settings.reduced_motion ? 1.0f : burst_from);
        burst_.target = 1.0f;
    }

    const Row *focused_row() const
    {
        const int f = rows_.focus();
        if (f < 0 || f >= static_cast<int>(view_.rows.size()))
            return nullptr;
        return &view_.rows[static_cast<std::size_t>(f)];
    }

    void handle_explore(const InputFrame &input, app::Feedback &feedback)
    {
        const VolumeState &v = volumes_[current_volume_];
        if (input.is_pressed(Action::menu))
        {
            menu_.set_focus(0);
            menu_.open({kMargin, 120.0f, 260.0f, 60.0f}, feedback);
            return;
        }
        if (input.is_pressed(Action::page_next) || input.is_pressed(Action::page_prev))
        {
            switch_volume(input.is_pressed(Action::page_next) ? 1 : -1, feedback);
            return;
        }
        if (input.is_pressed(Action::west))
        {
            start_scan(current_volume_, feedback);
            return;
        }
        if (input.is_pressed(Action::north))
        {
            by_name_ = !by_name_;
            const Row *row = focused_row();
            set_centre(view_.centre, 1.0f, row != nullptr ? row->node : dk::kNoNode);
            feedback.play(audio::Cue::tab);
            return;
        }
        if (input.is_pressed(Action::back))
        {
            const std::uint32_t parent = v.tree->node(view_.centre).parent;
            feedback.play(audio::Cue::back);
            if (parent == dk::kNoNode)
                back_to_volumes(current_volume_);
            else
                set_centre(parent, 1.18f, view_.centre);
            return;
        }

        aim(input, feedback);

        if (rows_.handle(input, feedback) != ui::Event::activated)
            return;
        const Row *row = focused_row();
        if (row != nullptr && row->kind == dk::SegmentKind::node &&
            row->node_kind == dk::Kind::directory &&
            v.tree->node(row->node).first_child != dk::kNoNode)
        {
            feedback.play(audio::Cue::open);
            set_centre(row->node, 0.72f, dk::kNoNode);
        }
        else
        {
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            refusal_.trigger();
        }
    }

    // The right stick aims at a first-ring segment; the list follows.
    void aim(const InputFrame &input, app::Feedback &feedback)
    {
        const float sx = input.stick2_x;
        const float sy = input.stick2_y;
        if (sx * sx + sy * sy < 0.36f)
            return;
        const int s = dk::segment_at(view_.segments, std::atan2(sx, -sy));
        if (s < 0)
            return;
        const dk::Segment &seg = view_.segments[static_cast<std::size_t>(s)];
        for (std::size_t i = 0; i < view_.rows.size(); ++i)
        {
            const Row &r = view_.rows[i];
            const bool match = seg.kind == dk::SegmentKind::hidden
                                   ? r.kind == dk::SegmentKind::hidden
                                   : seg.kind == dk::SegmentKind::node && r.node == seg.node;
            if (match && rows_.focus() != static_cast<int>(i))
            {
                rows_.set_focus(static_cast<int>(i), false);
                feedback.play(audio::Cue::tick);
            }
        }
    }

    void switch_volume(int direction, app::Feedback &feedback)
    {
        const auto count = static_cast<int>(volumes_.size());
        if (count < 2)
        {
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            refusal_.trigger();
            return;
        }
        const auto next = static_cast<std::size_t>(
            (static_cast<int>(current_volume_) + direction + count) % count);
        feedback.play(audio::Cue::tab, 1.0f, direction > 0 ? 0.3f : -0.3f);
        volume_list_.set_focus(static_cast<int>(next));
        open_volume(next, feedback);
    }

    // --------------------------------------------------------------- menu

    void handle_menu(const InputFrame &input, app::Feedback &feedback)
    {
        if (menu_.handle(input, feedback) != ui::Event::activated)
            return;
        const int tag = menu_.items()[static_cast<std::size_t>(menu_.focus())].tag;
        if (tag == kMenuRescan)
            start_scan(current_volume_, feedback);
        else if (tag == kMenuDetails)
            show_details(feedback);
        else if (tag == kMenuAbout)
            show_about(feedback);
    }

    void show_details(app::Feedback &feedback)
    {
        const VolumeState &v = volumes_[current_volume_];
        const dk::Node &root = v.tree->node(v.tree->root());
        std::string body = v.volume.spec.name + " (" + v.volume.spec.path + ")\n";
        if (v.partial)
            body += "Partial scan: cancelled before it finished, not cached.\n";
        else
            body += "Scanned " + dk::format_date(v.info.scanned_at) + " UTC in " +
                    dk::format_duration(static_cast<double>(v.info.duration_ms) / 1000.0) + ".\n";
        body += dk::format_count(root.files) + " files in " + dk::format_count(root.folders) +
                " folders: " + dk::format_bytes(root.disk) + " on disk, " +
                dk::format_bytes(root.apparent) + " apparent.\n";
        if (v.volume.has_space)
            body += "The volume reports " + dk::format_bytes(v.volume.space.used) + " used of " +
                    dk::format_bytes(v.volume.space.total) + ".\n";
        const std::size_t unreadable =
            std::max<std::size_t>(v.unreadable.size(), static_cast<std::size_t>(v.info.unreadable));
        if (unreadable == 0)
            body += "Every folder could be read.";
        else
        {
            body += dk::format_count(unreadable) + (unreadable == 1 ? " folder" : " folders") +
                    " could not be read";
            for (std::size_t i = 0; i < v.unreadable.size() && i < 3; ++i)
                body += (i == 0 ? ": " : ", ") + v.unreadable[i];
            body += v.unreadable.size() > 3 ? ", ..." : ".";
        }
        dialog_.open(
            {ui::StatusKind::info, "Scan details", body, {{"OK", ui::ButtonKind::primary}}},
            feedback);
    }

    void show_about(app::Feedback &feedback)
    {
        std::string body =
            "Shows where your storage went: scan a volume, then explore it as rings and a "
            "sorted list.\nRead-only: it never changes your files. It writes only its own scan "
            "cache.\nGPL-3.0-or-later, built on ps5-homebrew-ui.";
        if (degraded())
            body += "\nElevation: " + env_.elevation.diagnostic;
        if (!last_error_.empty())
            body += "\nLast cache error: " + last_error_;
        dialog_.open(
            {ui::StatusKind::none, "Prospero Disk", body, {{"OK", ui::ButtonKind::primary}}},
            feedback);
    }

    // ------------------------------------------------------------ drawing

    void draw_title(gfx::DrawList &list, std::string_view title, std::string_view subtitle) const
    {
        ui::text(list, context_.fonts.display, title, kMargin, 150, 64, kInk);
        if (!subtitle.empty())
            ui::text(list, context_.fonts.regular, subtitle, kMargin, 204, 28, kMuted);
    }

    void draw_hints(gfx::DrawList &list, std::span<const ui::Hint> hints) const
    {
        ui::draw_hints(list, context_.fonts, ui::GlyphStyle::dark(), hints.data(),
                       static_cast<int>(hints.size()), 1824, true);
    }

    static void draw_usage_bar(gfx::DrawList &list, const Rect &r, std::uint64_t used,
                               std::uint64_t total, Color fill)
    {
        list.rounded_rect(r, r.h * 0.5f, Color::rgb(0xffffff, 0.08f));
        if (total == 0 || used == 0)
            return;
        const float share = std::clamp(
            static_cast<float>(static_cast<double>(used) / static_cast<double>(total)), 0.0f, 1.0f);
        list.rounded_rect({r.x, r.y, std::max(r.h, r.w * share), r.h}, r.h * 0.5f, fill);
    }

    static void draw_kind_icon(gfx::DrawList &list, float cx, float cy, dk::VolumeKind kind,
                               Color c)
    {
        const Color dark = Color::rgb(0x141830);
        switch (kind)
        {
        case dk::VolumeKind::internal:
            list.rounded_rect({cx - 26, cy - 18, 52, 36}, 7, c.with_alpha(0.25f));
            list.ring(cx - 4, cy, 10, 3, c);
            list.circle(cx + 16, cy + 9, 3, c);
            break;
        case dk::VolumeKind::extended:
            list.rounded_rect({cx - 30, cy - 10, 60, 20}, 4, c.with_alpha(0.3f));
            for (int i = 0; i < 4; ++i)
                list.rounded_rect({cx - 22 + static_cast<float>(i) * 12, cy - 5, 8, 10}, 2, c);
            break;
        case dk::VolumeKind::usb:
            list.rounded_rect({cx - 13, cy - 6, 26, 30}, 5, c.with_alpha(0.3f));
            list.rounded_rect({cx - 9, cy - 22, 18, 16}, 2, c);
            list.rounded_rect({cx - 5, cy - 18, 3, 5}, 1, dark);
            list.rounded_rect({cx + 2, cy - 18, 3, 5}, 1, dark);
            break;
        case dk::VolumeKind::folder:
            list.rounded_rect({cx - 26, cy - 14, 52, 32}, 6, c.with_alpha(0.3f));
            list.rounded_rect({cx - 26, cy - 20, 22, 10}, 4, c);
            break;
        }
    }

    void draw_volume_card(ui::Canvas &canvas, const Rect &row, const ui::ListItem &item) const
    {
        gfx::DrawList &list = canvas.list;
        const VolumeState &v = volumes_[static_cast<std::size_t>(item.tag)];
        const Color tint = v.volume.spec.kind == dk::VolumeKind::usb ? kWarn : kAccent;
        draw_kind_icon(list, row.x + 66, row.y + 62, v.volume.spec.kind, tint);
        ui::text(list, context_.fonts.semibold, v.volume.spec.name, row.x + 124, row.y + 54, 34,
                 kInk);
        const float name_w = context_.fonts.semibold.measure(v.volume.spec.name, 34);
        const std::string kind = kind_label(v.volume.spec.kind);
        ui::text(list, context_.fonts.regular,
                 kind == v.volume.spec.name ? v.volume.spec.path : kind + kDot + v.volume.spec.path,
                 row.x + 144 + name_w, row.y + 54, 24, kFaint);

        std::string status = "Not scanned";
        if (v.tree && v.partial)
            status = "Partial scan";
        else if (v.tree || v.cached)
            status = "Scanned " + dk::format_age(wall_seconds() - v.info.scanned_at);
        ui::text(list, context_.fonts.regular, status, row.x + row.w - 36, row.y + 54, 24,
                 v.tree || v.cached ? kMuted : kFaint, gfx::Align::right);

        if (!v.volume.has_space)
        {
            ui::text(list, context_.fonts.regular, "Size unknown", row.x + 124, row.y + 118, 24,
                     kFaint);
            return;
        }
        draw_usage_bar(list, {row.x + 124, row.y + 80, row.w - 160, 14}, v.volume.space.used,
                       v.volume.space.total, tint);
        ui::text(list, context_.fonts.regular, dk::format_bytes(v.volume.space.used) + " used",
                 row.x + 124, row.y + 128, 24, kMuted);
        ui::text(list, context_.fonts.regular,
                 dk::format_bytes(v.volume.space.free) + " free of " +
                     dk::format_bytes(v.volume.space.total),
                 row.x + row.w - 36, row.y + 128, 24, kFaint, gfx::Align::right);
    }

    void draw_volumes(ui::Canvas &canvas) const
    {
        gfx::DrawList &list = canvas.list;
        draw_title(list, "Prospero Disk", "Choose a volume to see what is using its space.");
        if (degraded())
        {
            // Degraded mode (PRD 5.1): say what is missing, offer what is readable.
            const Rect r{kMargin, 240, 1100, 54};
            list.rounded_rect(r, 14, Color::rgb(0xffb35c, 0.16f));
            ui::text(list, context_.fonts.semibold, "Limited access.", r.x + 22, r.y + 36, 24,
                     kWarn);
            ui::text(list, context_.fonts.regular,
                     "Start the ELF loader on port 9021 or the Lapy service, then relaunch.",
                     r.x + 214, r.y + 36, 24, kInk);
        }
        if (volumes_.empty())
        {
            ui::text(list, context_.fonts.semibold, "No readable volumes", kMargin, 380, 36, kInk);
            ui::text(list, context_.fonts.regular,
                     "Prospero Disk could not open any storage. Check the elevation and relaunch.",
                     kMargin, 430, 26, kMuted);
            return;
        }
        volume_list_.draw(canvas);

        const VolumeState &v = volumes_[static_cast<std::size_t>(volume_list_.focus())];
        const ui::Hint scanned[] = {{ui::Button::cross, "Open"},
                                    {ui::Button::square, "Rescan"},
                                    {ui::Button::options, "About"}};
        const ui::Hint fresh[] = {{ui::Button::cross, "Scan"}, {ui::Button::options, "About"}};
        if (v.tree || v.cached)
            draw_hints(list, scanned);
        else
            draw_hints(list, fresh);
    }

    void draw_loading(ui::Canvas &canvas) const
    {
        const VolumeState &v = volumes_[loading_volume_];
        draw_title(canvas.list, v.volume.spec.name, "Opening the last scan...");
        canvas.list.arc(960, 600, 60, 8, clock_ * 5.0f, 1.6f, kAccent);
    }

    // The rings around (cx, cy). A focused branch (or the hidden share) stays
    // bright and the rest dims, so the list and the picture point at one thing.
    void draw_sunburst(gfx::DrawList &list, const View &view, float cx, float cy, int focus_branch,
                       bool focus_hidden) const
    {
        constexpr float kCoreRadius = 104.0f;
        constexpr float kRing = 52.0f;
        constexpr float kGap = 5.0f;
        const float grow = std::max(0.0f, burst_.value);
        const float settle = std::clamp(1.0f - std::fabs(grow - 1.0f) * 2.5f, 0.0f, 1.0f);
        const bool any_focus = focus_branch >= 0 || focus_hidden;
        list.push_opacity(0.2f + 0.8f * settle);
        list.push_transform(grow, cx, cy, 0.0f, 0.0f);
        for (const dk::Segment &s : view.segments)
        {
            const float outer = kCoreRadius + static_cast<float>(s.ring) * (kRing + kGap);
            const float sweep = s.sweep - std::min(0.012f, s.sweep * 0.25f);
            if (sweep <= 0.0005f)
                continue;
            const bool lit = !any_focus || (s.kind == dk::SegmentKind::hidden
                                                ? focus_hidden
                                                : s.branch == focus_branch && s.branch >= 0);
            const float alpha = lit ? 1.0f : 0.4f;
            if (s.kind == dk::SegmentKind::hidden)
            {
                // Hatched: short dashes of two greys along the arc.
                constexpr float kDash = 0.045f;
                const int dashes = static_cast<int>(std::ceil(sweep / kDash));
                for (int i = 0; i < dashes; ++i)
                {
                    const float a = static_cast<float>(i) * kDash;
                    list.arc(cx, cy, outer, kRing, s.start + a, std::min(kDash, sweep - a),
                             (i % 2 == 0 ? kHiddenA : kHiddenB).with_alpha(alpha), false);
                }
                continue;
            }
            const Color c =
                s.kind == dk::SegmentKind::smaller ? kSmaller : branch_colour(s.branch, s.ring);
            list.arc(cx, cy, outer, kRing, s.start, sweep, c.with_alpha(alpha), false);
        }
        list.circle(cx, cy, kCoreRadius - 6.0f, kCore);
        list.ring(cx, cy, kCoreRadius - 6.0f, 2.0f, Color::rgb(0xffffff, 0.08f));
        list.pop_transform();
        list.pop_opacity();
    }

    void draw_scanning(ui::Canvas &canvas) const
    {
        gfx::DrawList &list = canvas.list;
        const VolumeState &v = volumes_[scanning_volume_];
        const dk::ScanProgress p = scanner_.progress();
        draw_title(list, "Scanning " + v.volume.spec.name, v.volume.spec.path);

        constexpr float cx = 540.0f;
        constexpr float cy = 600.0f;
        if (!scanning_view_.segments.empty())
            draw_sunburst(list, scanning_view_, cx, cy, -1, false);
        else
            list.circle(cx, cy, 98, kCore);
        list.arc(cx, cy, 104, 5, clock_ * 4.0f, 1.2f, kAccent.with_alpha(0.85f));
        ui::text(list, context_.fonts.semibold, dk::format_bytes(p.bytes), cx, cy + 10, 30, kInk,
                 gfx::Align::center);

        constexpr float x = 1080.0f;
        constexpr float w = 744.0f;
        const std::pair<const char *, std::string> stats[] = {
            {"Files", dk::format_count(p.files)},
            {"Folders", dk::format_count(p.folders)},
            {"Found", dk::format_bytes(p.bytes)},
            // The screen's clock: real time on the console, tour time on a PC.
            {"Elapsed", dk::format_duration(static_cast<double>(scan_age_))},
        };
        for (int i = 0; i < 4; ++i)
        {
            const float y = 330.0f + static_cast<float>(i) * 104.0f;
            ui::text(list, context_.fonts.regular, stats[i].first, x, y, 26, kMuted);
            ui::text(list, context_.fonts.mono, stats[i].second, x + w, y, 44, kInk,
                     gfx::Align::right);
            list.rounded_rect({x, y + 30, w, 2}, 1, Color::rgb(0xffffff, 0.07f));
        }
        if (v.volume.has_space && v.volume.space.used != 0)
        {
            draw_usage_bar(list, {x, 760, w, 12}, p.bytes, v.volume.space.used, kAccent);
            ui::text(list, context_.fonts.regular,
                     dk::format_share(p.bytes, v.volume.space.used) + " of the used space found", x,
                     806, 24, kMuted);
        }
        ui::text(list, context_.fonts.regular, context_.fonts.regular.font->fit(p.current, 22, w),
                 x, 860, 22, kFaint);

        const ui::Hint hints[] = {{ui::Button::circle, "Cancel"}};
        draw_hints(list, hints);
    }

    std::string title_name(std::string_view name) const
    {
        if (dk::is_title_id(name))
        {
            const auto it = titles_by_id_.find(std::string(name));
            if (it != titles_by_id_.end() && !it->second.name.empty())
                return it->second.name;
        }
        return std::string(name);
    }

    std::string display_name(const Row &r) const
    {
        return r.title_id.empty() ? r.name : title_name(r.title_id);
    }

    const TitleState *title_of(const Row &r) const
    {
        if (r.title_id.empty())
            return nullptr;
        const auto it = titles_by_id_.find(r.title_id);
        return it == titles_by_id_.end() ? nullptr : &it->second;
    }

    std::string breadcrumb() const
    {
        const VolumeState &v = volumes_[current_volume_];
        std::vector<std::string> parts;
        for (std::uint32_t at = view_.centre; at != v.tree->root() && at != dk::kNoNode;
             at = v.tree->node(at).parent)
            parts.push_back(title_name(v.tree->name(at)));
        std::string out = v.volume.spec.name;
        for (auto it = parts.rbegin(); it != parts.rend(); ++it)
            out += "  /  " + *it;
        return out;
    }

    void draw_row(ui::Canvas &canvas, const Rect &row, const ui::ListItem &item, float focus) const
    {
        gfx::DrawList &list = canvas.list;
        const Row &r = view_.rows[static_cast<std::size_t>(item.tag)];
        const std::uint64_t whole = view_.centre_disk + view_.hidden;
        const float share =
            whole == 0
                ? 0.0f
                : static_cast<float>(static_cast<double>(r.disk) / static_cast<double>(whole));

        // The proportional bar behind the row.
        const Color c = r.kind == dk::SegmentKind::hidden ? kHiddenB
                        : r.branch >= 0                   ? branch_colour(r.branch, 1)
                                                          : kSmaller;
        list.rounded_rect({row.x + 4, row.y + 6, std::max(8.0f, (row.w - 8) * share), row.h - 12},
                          10, c.with_alpha(0.16f + 0.10f * focus));

        // Leading: the title's icon over a swatch in the segment's colour.
        const float lx = row.x + 40;
        const float ly = row.y + row.h * 0.5f;
        list.rounded_rect({lx - 22, ly - 22, 44, 44}, 10, c.with_alpha(0.9f));
        const TitleState *title = title_of(r);
        if (title != nullptr && title->texture != 0)
            list.image(title->texture, {lx - 22, ly - 22, 44, 44}, {0, 0, 1, 1},
                       Color::rgb(0xffffff, std::clamp(title->age / 0.35f, 0.0f, 1.0f)), 10);
        else if (r.node_kind == dk::Kind::directory && r.kind == dk::SegmentKind::node)
        {
            list.rounded_rect({lx - 12, ly - 6, 24, 15}, 3, Color::rgb(0x141830, 0.55f));
            list.rounded_rect({lx - 12, ly - 10, 10, 6}, 2, Color::rgb(0x141830, 0.55f));
        }

        const ui::FontRef &bold = context_.fonts.semibold;
        ui::text(list, bold, bold.font->fit(display_name(r), 26, row.w - 330), row.x + 82,
                 row.y + 31, 26, kInk);
        std::string sub;
        if (r.kind == dk::SegmentKind::hidden)
            sub = "Used space the scan cannot see";
        else if (!r.title_id.empty())
            sub = r.title_id;
        else if (r.node_kind == dk::Kind::other_files)
            sub = dk::format_count(r.files) + " smaller files";
        else if (r.node_kind == dk::Kind::file)
            sub = "File";
        else if ((r.flags & dk::kUnreadable) != 0)
            sub = "Could not be read";
        else
            sub = dk::format_count(r.files) + (r.files == 1 ? " file" : " files");
        sub += kDot + dk::format_share(r.disk, whole);
        ui::text(list, context_.fonts.regular, sub, row.x + 82, row.y + 58, 20, kMuted);
        ui::text(list, context_.fonts.mono, dk::format_bytes(r.disk), row.x + row.w - 26,
                 row.y + 44, 25, kInk, gfx::Align::right);
    }

    void draw_detail(gfx::DrawList &list, const Row &r) const
    {
        const VolumeState &v = volumes_[current_volume_];
        const Rect panel{1010, 764, 814, 196};
        list.rounded_rect(panel, 22, Color::rgb(0x1b2036, 0.86f));
        float x = panel.x + 28;
        const TitleState *title = title_of(r);
        if (title != nullptr && title->texture != 0)
        {
            list.image(title->texture, {x, panel.y + 28, 140, 140}, {0, 0, 1, 1},
                       Color::rgb(0xffffff, std::clamp(title->age / 0.35f, 0.0f, 1.0f)), 18);
            x += 164;
        }
        const float w = panel.x + panel.w - 28 - x;
        const ui::FontRef &bold = context_.fonts.semibold;
        const ui::FontRef &regular = context_.fonts.regular;
        ui::text(list, bold, bold.font->fit(display_name(r), 30, w), x, panel.y + 54, 30, kInk);
        if (r.kind == dk::SegmentKind::hidden)
        {
            ui::text(list, regular, "Used on the volume but not found by the scan: the system,", x,
                     panel.y + 94, 22, kMuted);
            ui::text(list, regular, "other users' data and folders that could not be read.", x,
                     panel.y + 126, 22, kMuted);
            ui::text(list, context_.fonts.mono, dk::format_bytes(r.disk), x, panel.y + 166, 24,
                     kInk);
            return;
        }
        std::string line1 = r.title_id.empty() ? "" : r.title_id + kDot;
        line1 += dk::format_bytes(r.disk) + " on disk" + kDot + dk::format_bytes(r.apparent) +
                 " apparent";
        ui::text(list, regular, regular.font->fit(line1, 22, w), x, panel.y + 92, 22, kMuted);
        std::string line2 = "One file";
        if (r.node_kind == dk::Kind::directory)
            line2 =
                dk::format_count(r.files) + " files, " + dk::format_count(r.folders) + " folders";
        else if (r.node_kind == dk::Kind::other_files)
            line2 = dk::format_count(r.files) + " files, each smaller than those listed";
        if (r.mtime != 0)
            line2 += kDot + std::string("modified ") + dk::format_date(r.mtime);
        ui::text(list, regular, regular.font->fit(line2, 22, w), x, panel.y + 126, 22, kMuted);
        const std::string path = r.node != dk::kNoNode ? v.tree->path(r.node) : view_.path;
        ui::text(list, regular, regular.font->fit(path, 20, w), x, panel.y + 164, 20, kFaint);
    }

    void draw_explore(ui::Canvas &canvas) const
    {
        gfx::DrawList &list = canvas.list;
        const VolumeState &v = volumes_[current_volume_];

        // Header: volume, its size and its statfs bar.
        std::string title = v.volume.spec.name;
        if (v.volume.has_space)
            title += kDot + dk::format_bytes(v.volume.space.total);
        ui::text(list, context_.fonts.display, title, kMargin, 110, 46, kInk);
        if (v.partial)
        {
            const float tw = context_.fonts.display.measure(title, 46);
            list.rounded_rect({kMargin + tw + 24, 76, 150, 40}, 20, Color::rgb(0xffb35c, 0.2f));
            ui::text(list, context_.fonts.semibold, "Partial", kMargin + tw + 99, 104, 22, kWarn,
                     gfx::Align::center);
        }
        if (v.volume.has_space)
        {
            draw_usage_bar(list, {1010, 82, 814, 12}, v.volume.space.used, v.volume.space.total,
                           kAccent);
            ui::text(list, context_.fonts.regular,
                     dk::format_bytes(v.volume.space.used) + " used" + kDot +
                         dk::format_bytes(v.volume.space.free) + " free",
                     1824, 128, 22, kMuted, gfx::Align::right);
        }
        ui::text(list, context_.fonts.regular,
                 context_.fonts.regular.font->fit(breadcrumb(), 26, 860), kMargin, 166, 26, kMuted);

        const Row *focus = focused_row();
        int focus_branch = -1;
        bool focus_hidden = false;
        if (focus != nullptr)
        {
            focus_hidden = focus->kind == dk::SegmentKind::hidden;
            // A row too small for a segment of its own lights no branch: all dim.
            constexpr int kNoBranch = 1000;
            if (focus->kind == dk::SegmentKind::node)
                focus_branch = focus->branch >= 0 ? focus->branch : kNoBranch;
        }
        constexpr float cx = 520.0f;
        constexpr float cy = 600.0f;
        const float x = cx + ui::shake(refusal_.value, clock_);
        draw_sunburst(list, view_, x, cy, focus_branch, focus_hidden);
        const std::string centre =
            view_.centre == v.tree->root() ? v.volume.spec.name : title_name(view_.centre_name);
        const ui::FontRef &bold = context_.fonts.semibold;
        ui::text(list, bold, bold.font->fit(centre, 28, 170), x, cy - 6, 28, kInk,
                 gfx::Align::center);
        ui::text(list, context_.fonts.regular, dk::format_bytes(view_.centre_disk + view_.hidden),
                 x, cy + 30, 24, kMuted, gfx::Align::center);

        ui::text(list, bold, by_name_ ? "NAME  (sorted)" : "NAME", 1036, 186, 18, kFaint);
        ui::text(list, bold, by_name_ ? "SIZE" : "SIZE  (sorted)", 1798, 186, 18, kFaint,
                 gfx::Align::right);
        if (view_.rows.empty())
            ui::text(list, context_.fonts.regular, "This folder is empty.", 1040, 260, 26, kMuted);
        else
            rows_.draw(canvas);
        if (focus != nullptr)
            draw_detail(list, *focus);

        const ui::Hint hints[] = {
            {ui::Button::cross, "Open"},    {ui::Button::circle, "Up"},
            {ui::Button::square, "Rescan"}, {ui::Button::triangle, "Sort"},
            {ui::Button::options, "Menu"},  {ui::Button::l1, "Volume", ui::Button::r1}};
        draw_hints(list, hints);
    }

    // ---------------------------------------------------------------- state

    app::Context &context_;
    dk::Environment env_;
    dk::TitleLoader titles_;
    std::map<std::string, TitleState> titles_by_id_;
    std::vector<VolumeState> volumes_;
    dk::Scanner scanner_;
    std::unique_ptr<LoadJob> load_;
    std::unique_ptr<SaveJob> save_;
    std::string last_error_;

    Screen screen_ = Screen::volumes;
    std::size_t current_volume_ = 0;
    std::size_t scanning_volume_ = 0;
    std::size_t loading_volume_ = 0;
    View view_;
    View scanning_view_;
    float scan_refresh_ = 0.0f;
    float scan_age_ = 0.0f;
    bool by_name_ = false;

    ui::ListView volume_list_;
    ui::ListView rows_;
    ui::Menu menu_;
    ui::Dialog dialog_;
    tween::Spring burst_;
    ui::Pulse refusal_;
    float clock_ = 0.0f;
};

} // namespace

std::unique_ptr<app::Concept> make_disk(app::Context &context)
{
    return std::make_unique<Disk>(context);
}

} // namespace hui::concepts
