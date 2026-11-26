// ps5-homebrew-ui - Application entry point.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Opens the display, controller and audio, then runs the shell every frame:
// read input, update, play the sounds it asked for, draw, present. Lifecycle
// markers and frame pacing are logged for hardware runs.

#include "app/shell.hpp"
#include "disk/deleter.hpp"
#include "disk/volumes.hpp"
#include "app/tour.hpp"
#include "audio/cues.hpp"
#include "audio/mixer.hpp"
#include "audio/music.hpp"
#include "core/frame_stats.hpp"
#include "core/input.hpp"
#include "core/save_file.hpp"
#include "core/settings.hpp"
#include "core/version.hpp"
#include "demo/catalog.hpp"
#include "gfx/canvas.hpp"
#include "gfx/renderer.hpp"
#include "gfx/system_fonts.hpp"
#include "platform/ps5/audio_out.hpp"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/storage_probe.hpp"
#include "platform/ps5/write_probe.hpp"
#include "platform/ps5/elevation/elevation.hpp"
#include "platform/ps5/system.hpp"
#include "platform/ps5/system_language.hpp"
#include "ui/fonts.hpp"

#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <GL/glcorearb.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

extern "C" void hui_heap_stats(std::size_t *live_bytes, std::size_t *peak_bytes,
                               std::size_t *blocks, std::size_t *failures);

namespace
{

// Set when the effective uid could not be put back after a removal: the
// process is root from then on, so the main loop stops too.
std::atomic<bool> g_uid_lost{false};

const char *elevation_status_name(elevation::Status status)
{
    switch (status)
    {
    case elevation::Status::ok:
        return "ok";
    case elevation::Status::invalid_request:
        return "invalid request";
    case elevation::Status::unsupported_version:
        return "unsupported version";
    case elevation::Status::unsupported_capability:
        return "unsupported capability";
    case elevation::Status::target_mismatch:
        return "target mismatch";
    case elevation::Status::unavailable:
        return "unavailable (no ELF loader on port 9021 and no Lapy service)";
    case elevation::Status::prepare_failed:
        return "prepare failed";
    case elevation::Status::apply_failed:
        return "apply failed";
    case elevation::Status::rollback_failed:
        return "rollback failed";
    case elevation::Status::transport_error:
        return "transport error";
    case elevation::Status::protocol_error:
        return "protocol error";
    case elevation::Status::timeout:
        return "timeout";
    }
    return "unknown";
}

// Where the app's own files are. Before the Lapy grant the title sees its
// sandbox: the install folder is /app0 and its save area /download0. The grant
// swaps the process to the console's root view, where neither path exists
// (docs/platform/CONSOLE_NOTES.md): the install folder is then
// /data/homebrew/<TITLE_ID>, and the app keeps its data in /data/prospero-disk.
struct AppPaths
{
    std::string app;   // the install folder: assets/, sce_sys/, dev/request.txt
    std::string data;  // settings
    std::string dev;   // development output (the tour's pictures and report)
    std::string probe; // probe.txt
};

// The sandbox layout, used when the grant fails (and on the first lines).
// Settings live in the title's own save area. A PC can *read*
// /mnt/sandbox/<TITLE_ID>_000/download0/hui/dev over FTP while the title runs,
// but cannot write there; requests therefore come in through the install
// folder: /data/homebrew/<TITLE_ID>/dev/request.txt. One line:
// "tour <design id|all> <token>" or "quit - <token>". A request is honoured
// once per token (the last one is remembered in the dev folder).
AppPaths sandbox_paths()
{
    return {"/app0", "/download0/hui", "/download0/hui/dev", "/download0/prospero-disk"};
}

constexpr const char *kGrantedDataRoot = "/data/prospero-disk";

// The value of a top-level string key in param.json text ("" when absent).
std::string json_string(const std::string &text, const char *key)
{
    const std::string quoted = std::string("\"") + key + "\"";
    const std::size_t at = text.find(quoted);
    if (at == std::string::npos)
        return {};
    const std::size_t colon = text.find(':', at + quoted.size());
    const std::size_t open = colon == std::string::npos ? colon : text.find('"', colon);
    const std::size_t close = open == std::string::npos ? open : text.find('"', open + 1);
    return close == std::string::npos ? std::string() : text.substr(open + 1, close - open - 1);
}

// After the grant: the first candidate install folder that holds the fonts.
// "" when none does (the log then names every candidate).
std::string find_app_root(const std::string &title_id)
{
    std::vector<std::string> candidates = {"/app0"};
    if (!title_id.empty())
    {
        candidates.push_back("/data/homebrew/" + title_id);
        candidates.push_back("/mnt/sandbox/" + title_id + "_000/app0");
    }
    for (const std::string &root : candidates)
    {
        const std::string probe = root + "/assets/fonts/inter-regular.huifont";
        const bool ok = access(probe.c_str(), R_OK) == 0;
        hui::sys::log("[HUI] app root candidate %s: %s", root.c_str(),
                      ok ? "ok" : std::strerror(errno));
        if (ok)
            return root;
    }
    return {};
}

// Creates a folder for the app under /data. /data and what an FTP upload
// leaves there are root's, and root has no DAC bypass on this console, so the
// folder is made with the effective uid raised and then opened (0777) for the
// app's own user and for FTP. Call only while no other thread runs: the
// effective uid is per process.
bool ensure_granted_directory(const std::string &path, uid_t app_user)
{
    const bool raise = geteuid() != 0 && seteuid(0) == 0;
    bool ok = hui::save::ensure_directory(path);
    int error = errno;
    if (ok && chmod(path.c_str(), 0777) != 0)
        hui::sys::log("[HUI] chmod %s failed: %s", path.c_str(), std::strerror(errno));
    if (raise && seteuid(app_user) != 0)
    {
        // Everything after this would run as root and leave root-owned files
        // the app cannot change later: stop before any thread starts.
        hui::sys::log("[HUI] fatal: effective uid restore failed after %s: %s", path.c_str(),
                      std::strerror(errno));
        hui::sys::park();
    }
    if (!ok)
        hui::sys::log("[HUI] directory %s failed: %s", path.c_str(), std::strerror(error));
    return ok;
}
// How long a finished tour stays up so the PC can copy its evidence (the
// storage is unmounted when the title closes) before the app closes itself.
constexpr double kCollectSeconds = 240.0;
// Quarter of a quarter: the title's storage is small and a tour takes
// hundreds of pictures. Enough to compare layout and colour with the PC's.
constexpr int kCaptureWidth = 480;
constexpr int kCaptureHeight = 270;

void log_heap(std::uint64_t frames)
{
    std::size_t live = 0;
    std::size_t peak = 0;
    std::size_t blocks = 0;
    std::size_t failures = 0;
    hui_heap_stats(&live, &peak, &blocks, &failures);
    hui::sys::log("[HUI] heap frames=%llu live=%zu peak=%zu blocks=%zu failures=%zu",
                  static_cast<unsigned long long>(frames), live, peak, blocks, failures);
}

// Opens at *resolution (an index into Settings::kResolutions), falling back
// to 1080p; *resolution reports the mode that opened.
bool open_display(hui::ps5::Display &display, int *resolution)
{
    using hui::Settings;
    const Settings::Resolution &mode = Settings::kResolutions[*resolution];
    if (display.open(mode.width, mode.height))
    {
        hui::sys::log("[HUI] display mode %s %dx%d", mode.label, display.width(), display.height());
        return true;
    }
    if (*resolution == 0)
        return false;
    hui::sys::log("[HUI] display mode %s failed, using 1080p", mode.label);
    *resolution = 0;
    return display.open(Settings::kResolutions[0].width, Settings::kResolutions[0].height);
}

bool load_font(hui::gfx::Renderer &renderer, const std::string &assets, const char *name,
               hui::gfx::Font *font, hui::ui::FontRef *ref, bool system_fonts = false)
{
    std::string data;
    const std::string path = assets + "/fonts/" + name;
    if (!hui::save::read_file(path, &data))
    {
        hui::sys::log("[HUI] font %s unreadable: %s", path.c_str(), std::strerror(errno));
        return false;
    }
    if (!font->load(data))
    {
        hui::sys::log("[HUI] font %s failed: %s", name, font->error().c_str());
        return false;
    }
    ref->font = font;
    // The faces that draw names (files, titles) also take other scripts from
    // the console's own fonts (docs/PRD.md 7.5), before the texture exists.
    const std::size_t extra =
        system_fonts ? hui::gfx::attach_system_fonts(*font, hui::ps5::current_language_tag()) : 0;
    ref->texture = renderer.batch().create_font_texture(*font);
    if (extra != 0)
        renderer.track_font(ref->texture, font);
    return true;
}

// The frame as drawn, bottom row first, as a 24-bit BMP (tour pictures).
bool save_picture(const std::string &path, int width, int height)
{
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    const std::uint32_t row = (static_cast<std::uint32_t>(width) * 3 + 3) & ~3u;
    const std::uint32_t size = 54 + row * static_cast<std::uint32_t>(height);
    unsigned char header[54] = {'B', 'M'};
    const auto put = [&](int at, std::uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
            header[at + i] = static_cast<unsigned char>(value >> (8 * i));
    };
    put(2, size);
    put(10, 54);
    put(14, 40);
    put(18, static_cast<std::uint32_t>(width));
    put(22, static_cast<std::uint32_t>(height));
    header[26] = 1;
    header[28] = 24;
    put(34, size - 54);
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
        return false;
    bool ok = std::fwrite(header, 1, sizeof(header), file) == sizeof(header);
    std::vector<unsigned char> line(row);
    for (int y = 0; ok && y < height; ++y)
    {
        const unsigned char *in = pixels.data() + static_cast<std::size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x)
        {
            line[static_cast<std::size_t>(x) * 3 + 0] = in[x * 4 + 2];
            line[static_cast<std::size_t>(x) * 3 + 1] = in[x * 4 + 1];
            line[static_cast<std::size_t>(x) * 3 + 2] = in[x * 4 + 0];
        }
        ok = std::fwrite(line.data(), 1, line.size(), file) == line.size();
    }
    return std::fclose(file) == 0 && ok;
}

} // namespace

int main()
{
    using namespace hui;
    sys::log("[HUI] entry");

    // What the install folder says about itself, read while /app0 still exists.
    std::string param_json;
    (void)save::read_file("/app0/sce_sys/param.json", &param_json);
    const std::string title_id = json_string(param_json, "titleId");
    const std::string version = content_version(param_json);
    sys::log("[HUI] title %s version %s", title_id.empty() ? "unknown" : title_id.c_str(),
             version.empty() ? "unknown" : version.c_str());

    // Sandbox elevation (docs/PRD.md 7.2): once, before EGL or any other
    // thread exists. A failure leaves the app in its degraded mode.
    const std::int64_t started_us = sys::monotonic_us();
    disk::Elevation elevation;
    elevation.attempted = true;
    const uid_t original_uid = geteuid();
    const bool helper_present = access("/app0/lapy.elf", R_OK) == 0;
    const elevation::Status grant = elevation::request(elevation::Capability::filesystem);
    elevation.granted = grant == elevation::Status::ok;
    // The client reports a missing helper file and a closed port 9021 alike.
    elevation.diagnostic =
        !helper_present && !elevation.granted
            ? std::string("helper file /app0/lapy.elf missing")
            : std::string(elevation_status_name(grant)) + " via " + elevation::path();
    if (elevation.granted)
    {
        // As Playden does: keep the root view the grant gives, but act as the
        // title's own user again, so that what the app writes is not owned by
        // root; then let the helper finish settling the process.
        if (geteuid() != original_uid && seteuid(original_uid) != 0)
            sys::log("[HUI] elevation: effective uid restore failed (%d)", errno);
        sys::sleep_us(1000000);
    }
    disk::set_elevation(elevation);
    if (elevation.granted)
    {
        // Removing an entry needs its folder's owner, and users' folders are
        // root's (docs/platform/CONSOLE_NOTES.md section 4): the deleter raises
        // the euid for one call at a time, after the app's own user was
        // refused. The euid is per process, so the screen starts no file
        // writes while a deletion runs.
        disk::set_privileged(
            [original_uid](const std::function<bool()> &call)
            {
                if (seteuid(0) != 0)
                    return false;
                const bool ok = call();
                const int error = errno;
                if (seteuid(original_uid) != 0)
                {
                    // Anything after this would run as root: stop this thread,
                    // and the main loop with it.
                    sys::log("[HUI] fatal: effective uid restore failed after a removal (%d)",
                             errno);
                    g_uid_lost = true;
                    sys::park();
                }
                errno = error;
                return ok;
            });
    }
    sys::log("[HUI] elevation %s after %lld ms, euid %d", elevation.diagnostic.c_str(),
             static_cast<long long>((sys::monotonic_us() - started_us) / 1000),
             static_cast<int>(geteuid()));
    // Where the app's files are from here on. Still single-threaded, so the
    // effective uid may be raised to create the /data folders.
    AppPaths paths = sandbox_paths();
    if (elevation.granted)
    {
        paths.app = find_app_root(title_id);
        const std::string data = kGrantedDataRoot;
        // Without the folder nothing is saved (settings, the probe, the tour
        // and its requests: an empty path turns each off), but the app runs.
        paths.data.clear();
        paths.dev.clear();
        paths.probe.clear();
        if (ensure_granted_directory(data, original_uid))
        {
            paths.data = data;
            paths.probe = data;
            (void)ensure_granted_directory(data + "/cache", original_uid);
            // Where a Playden title's folder waits while its title is uninstalled.
            (void)ensure_granted_directory(data + "/trash", original_uid);
            if (ensure_granted_directory(data + "/dev", original_uid))
                paths.dev = data + "/dev";
        }
    }
    if (geteuid() != original_uid)
    {
        sys::log("[HUI] fatal: effective uid %d, expected %d", static_cast<int>(geteuid()),
                 static_cast<int>(original_uid));
        sys::park();
    }
    sys::log("[HUI] paths app=%s data=%s dev=%s", paths.app.c_str(), paths.data.c_str(),
             paths.dev.c_str());
    if (paths.app.empty())
    {
        sys::log("[HUI] fatal: the install folder is not visible after the grant");
        sys::park();
    }
    const std::string assets = paths.app + "/assets";

    // What the elevation made readable (docs/PRD.md 10.1-3): to the log, and
    // beside the cache for FTP, in the app's own folder (PRD F11).
    {
        const std::string report = ps5::probe_storage(
            elevation.granted,
            [started_us](const std::string &text)
            {
                sys::log("[PD] %lld %s",
                         static_cast<long long>((sys::monotonic_us() - started_us) / 1000),
                         text.c_str());
            });
        const std::string &dir = paths.probe;
        if (!dir.empty() && save::ensure_directory(dir))
        {
            const std::string error = save::write_atomic(dir + "/probe.txt", report);
            if (!error.empty())
                sys::log("[HUI] probe.txt in %s failed: %s", dir.c_str(), error.c_str());
        }
    }
    // The write probe (docs/plans/collect-delete.md, section 2): only when
    // dev/write-probe was uploaded to the install folder, and once: its report
    // stays in the data folder until someone deletes it. Still single-threaded,
    // so the probe may raise the effective uid.
    if (elevation.granted && !paths.data.empty() &&
        access((paths.app + "/dev/write-probe").c_str(), F_OK) == 0 &&
        access((paths.data + "/write-probe.txt").c_str(), F_OK) != 0)
    {
        // Written first, so a probe that ends the process does not run again.
        (void)save::write_atomic(paths.data + "/write-probe.txt", "write probe started\n");
        const std::string report = ps5::probe_writes(
            paths.data, original_uid,
            [started_us](const std::string &text)
            {
                sys::log("[PW] %lld %s",
                         static_cast<long long>((sys::monotonic_us() - started_us) / 1000),
                         text.c_str());
            });
        const std::string error = save::write_atomic(paths.data + "/write-probe.txt", report);
        if (!error.empty())
            sys::log("[HUI] write-probe.txt failed: %s", error.c_str());
    }
    const bool data_ok = !paths.data.empty() && save::ensure_directory(paths.data);
    sys::log("[HUI] storage dir=%d%s%s", data_ok ? 1 : 0, data_ok ? "" : " ",
             data_ok ? "" : std::strerror(errno));

    // The display opens at the saved resolution (1080p if that fails).
    const Settings saved = app::Shell::load_settings(paths.data);
    int resolution = ps5::Display::supports_display_modes() ? saved.resolution : 0;
    ps5::Display display;
    if (!open_display(display, &resolution))
    {
        sys::log("[HUI] fatal: display open failed");
        sys::park();
    }

    gfx::Renderer renderer;
    gfx::Font regular;
    gfx::Font semibold;
    gfx::Font display_font;
    gfx::Font mono;
    gfx::Font pixel;
    gfx::Font hand;
    ui::Fonts fonts;
    if (!renderer.init() ||
        !load_font(renderer, assets, "inter-regular.huifont", &regular, &fonts.regular, true) ||
        !load_font(renderer, assets, "inter-semibold.huifont", &semibold, &fonts.semibold, true) ||
        !load_font(renderer, assets, "montserrat-medium.huifont", &display_font, &fonts.display) ||
        !load_font(renderer, assets, "dejavu-sans-mono.huifont", &mono, &fonts.mono) ||
        !load_font(renderer, assets, "press-start-2p.huifont", &pixel, &fonts.pixel) ||
        !load_font(renderer, assets, "patrick-hand.huifont", &hand, &fonts.hand))
    {
        sys::log("[HUI] fatal: renderer init failed");
        sys::park();
    }
    demo::Catalog catalog;
    const std::int64_t art_start = sys::monotonic_us();
    if (!catalog.build_covers(renderer, fonts))
        sys::log("[HUI] cover art failed; designs draw without it");
    sys::log("[HUI] cover art %zu covers in %lld ms", catalog.size(),
             static_cast<long long>((sys::monotonic_us() - art_start) / 1000));

    ps5::Pad pad;
    pad.open();
    InputTracker tracker;
    audio::Mixer mixer;
    // The music stream attaches to the mixer before the audio thread starts.
    audio::MusicPlayer music;
    const int tracks =
        music.init(mixer, assets + "/audio/music", static_cast<std::uint64_t>(sys::monotonic_us()));
    sys::log("[HUI] music songs=%d", tracks);
    ps5::AudioOut audio_out;
    audio_out.start(mixer);
    audio::SoundBank sounds;
    const auto bank = sounds.load(assets + "/audio/sfx");
    sys::log("[HUI] sounds files=%d rejected=%d", bank.files, bank.rejected);
    for (const std::string &error : bank.errors)
        sys::log("[HUI] sound rejected %s", error.c_str());

    app::Shell shell(fonts, catalog, paths.data, renderer.glass_texture());
    shell.set_texture_factory([&renderer](int width, int height, const std::uint8_t *rgba)
                              { return renderer.batch().create_texture(width, height, rgba); });
    shell.set_version(version);
    sys::log("[HUI] version %s designs=%zu", version.empty() ? "unknown" : version.c_str(),
             shell.concept_count());

    // A tour request turns this launch into an unattended validation run:
    // every design is shown with scripted input, pictures and a report are
    // written to the dev folder, and the app closes itself a while later.
    std::unique_ptr<app::Tour> tour;
    save::ensure_directory(paths.dev);
    const std::string handled_path = paths.dev + "/handled.txt";
    const std::string report_path = paths.dev + "/tour/report.txt";
    bool quit_requested = false;
    const auto poll_request = [&]()
    {
        if (paths.dev.empty())
            return;
        std::string request;
        if (!save::read_file(paths.app + "/dev/request.txt", &request, 256))
            return;
        char verb[16] = {};
        char argument[64] = {};
        char token[64] = {};
        if (std::sscanf(request.c_str(), "%15s %63s %63s", verb, argument, token) != 3)
            return;
        std::string handled;
        save::read_file(handled_path, &handled, 128);
        if (handled == token)
            return; // this request was honoured by an earlier launch (or frame)
        if (const std::string error = save::write_atomic(handled_path, token); !error.empty())
        {
            sys::log("[HUI] request %s ignored: %s cannot be written (%s)", token,
                     handled_path.c_str(), error.c_str());
            return;
        }
        if (std::strcmp(verb, "quit") == 0)
        {
            quit_requested = true;
        }
        else if (std::strcmp(verb, "tour") == 0 && !tour)
        {
            save::ensure_directory(paths.dev + "/tour");
            std::remove(report_path.c_str()); // a new report must not be taken for an old one
            const bool all = std::strcmp(argument, "all") == 0;
            tour = std::make_unique<app::Tour>(shell, all ? std::string() : std::string(argument));
            sys::log("[HUI] tour requested: %s token=%s", argument, token);
        }
    };
    poll_request();
    double collect_wait = -1.0; // seconds a finished tour has waited to be collected

    std::int64_t previous = sys::monotonic_us();
    std::uint64_t frames = 0;
    FrameStats stats;
    // The FPS readout averages over half a second so the number is readable.
    double fps_seconds = 0.0;
    int fps_frames = 0;
    PadSample samples[64];
    gfx::Canvas capture;
    std::int64_t last_frame_start = sys::monotonic_us();
    for (;;)
    {
        const std::int64_t now = sys::monotonic_us();
        // Animation time is start-to-start (one full frame), not the gap
        // between the previous swap returning and this frame beginning.
        float dt = frames == 0 ? 1.0f / 60.0f : static_cast<float>(now - last_frame_start) / 1e6f;
        last_frame_start = now;
        if (dt > 0.05f)
            dt = 0.05f; // a hitch must not teleport the animations
        const std::size_t count = pad.read(samples);
        InputFrame input = tracker.update(std::span<const PadSample>(samples, count),
                                          static_cast<std::uint64_t>(now));
        if (tour)
            input = tour->step(dt);

        shell.update(input, dt);
        if (shell.take_settings_changed())
        {
            const Settings &settings = shell.settings();
            mixer.set_bus_gain(audio::Bus::music, Settings::gain(settings.music_volume));
            mixer.set_bus_gain(audio::Bus::sfx, Settings::gain(settings.sfx_volume));
            mixer.set_bus_gain(audio::Bus::ui, Settings::gain(settings.ui_volume));
            InputSettings input_settings = tracker.settings();
            input_settings.swap_confirm = settings.swap_confirm;
            tracker.set_settings(input_settings);
        }
        const app::Feedback &feedback = shell.feedback();
        for (const audio::CueEvent &event : feedback.cues)
        {
            sounds.play(mixer, event.set == audio::SoundSet::count ? shell.sound_set() : event.set,
                        event);
            if (event.cue == audio::Cue::complete || event.cue == audio::Cue::welcome)
                music.duck();
        }
        if (shell.settings().haptics && feedback.rumble_strength > 0.0f)
            pad.rumble(feedback.rumble_strength, feedback.rumble_seconds);
        pad.tick(dt);
        if (shell.settings().light_bar)
        {
            const gfx::Color accent = shell.accent();
            pad.set_light_bar(static_cast<std::uint8_t>(accent.r * 255.0f),
                              static_cast<std::uint8_t>(accent.g * 255.0f),
                              static_cast<std::uint8_t>(accent.b * 255.0f));
        }
        music.pump(dt);

        shell.compose(renderer);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        renderer.present(0, display.width(), display.height());
        if (tour && !tour->capture().empty())
        {
            // The picture is the same frame drawn once more into a small
            // off-screen target: reading the display surface back is slow, and
            // a quarter-size picture is enough to compare with the PC render.
            if (capture.texture() == 0)
                capture.create(kCaptureWidth, kCaptureHeight, 1);
            capture.bind();
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            renderer.present(capture.framebuffer(), kCaptureWidth, kCaptureHeight);
            glBindFramebuffer(GL_FRAMEBUFFER, capture.framebuffer());
            const std::string path = paths.dev + "/tour/" + tour->capture() + ".bmp";
            const bool ok = save_picture(path, kCaptureWidth, kCaptureHeight);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            sys::log("[HUI] tour picture %s ok=%d", tour->capture().c_str(), ok ? 1 : 0);
            tour->capture_done();
            last_frame_start = sys::monotonic_us(); // saving is slow; the frame was not
        }
        if (!display.swap())
        {
            sys::log("[HUI] fatal: swap failed frame=%llu error=%s",
                     static_cast<unsigned long long>(frames),
                     ps5::egl_error_name(display.last_error()));
            sys::park();
        }
        ++frames;
        const std::int64_t presented = sys::monotonic_us();
        const float frame_ms = static_cast<float>(presented - previous) / 1000.0f;
        app::Telemetry &telemetry = shell.telemetry();
        if (frames == 1)
        {
            sys::log("[HUI] first-swap ok shapes=%zu draws=%zu", renderer.last_instances(),
                     renderer.last_draw_calls());
            const bool hidden = sys::hide_splash_screen();
            sys::log("[HUI] ready splash_hidden=%d", hidden ? 1 : 0);
            log_heap(frames);
        }
        else
        {
            stats.add(static_cast<double>(frame_ms));
            telemetry.push(frame_ms);
            if (tour)
                tour->record_frame(frame_ms, renderer.last_draw_calls(), renderer.last_instances());
        }
        previous = presented;
        telemetry.draw_calls = renderer.last_draw_calls();
        telemetry.instances = renderer.last_instances();
        telemetry.voices = mixer.active_voices();
        fps_seconds += static_cast<double>(frame_ms) / 1000.0;
        ++fps_frames;
        if (fps_seconds >= 0.5)
        {
            telemetry.fps = static_cast<float>(fps_frames / fps_seconds);
            telemetry.average_ms = static_cast<float>(fps_seconds * 1000.0 / fps_frames);
            fps_seconds = 0.0;
            fps_frames = 0;
        }

        if (tour && tour->finished())
        {
            std::string report;
            for (const std::string &line : tour->report())
            {
                sys::log("[HUI] %s", line.c_str());
                report += line + "\n";
            }
            save::write_atomic(report_path, report);
            sys::log("[HUI] tour finished designs=%zu", tour->report().size());
            // The storage is unmounted when the title closes, so the app stays
            // up for a while: the PC copies the evidence, then the app closes.
            tour.reset();
            collect_wait = 0.0;
        }
        if (collect_wait >= 0.0)
            collect_wait += static_cast<double>(frame_ms) / 1000.0;
        if (g_uid_lost)
        {
            sys::log("[HUI] fatal: parked, the effective uid is lost");
            sys::park();
        }
        // Requests write a file and may quit: not while a deletion runs.
        if (frames % 60 == 0 && !disk::deletion_running())
        {
            poll_request();
            if (quit_requested || collect_wait > kCollectSeconds)
            {
                sys::log("[HUI] closing: %s",
                         quit_requested ? "quit requested" : "tour evidence window over");
                pad.close();
                sys::quit();
            }
        }
        if (stats.count() == 600)
        {
            char summary[160];
            stats.format(summary, sizeof(summary));
            sys::log("[HUI] %s draws=%zu shapes=%zu", summary, renderer.last_draw_calls(),
                     renderer.last_instances());
            sys::log("[HUI] audio grains=%llu errors=%llu voices=%d music=%s underruns=%llu",
                     static_cast<unsigned long long>(audio_out.grains()),
                     static_cast<unsigned long long>(audio_out.errors()), mixer.active_voices(),
                     music.current().c_str(),
                     static_cast<unsigned long long>(mixer.stream_underruns()));
            stats.reset();
            if (frames % 3600 < 600)
                log_heap(frames);
        }
    }
}
