// Prospero Disk - Uninstalling a title through the system, and the fixture's stand-in.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "disk/deleter.hpp"
#include "disk/filesystem.hpp"

#include <functional>
#include <memory>
#include <string>

namespace hui::disk
{

// The app cannot load libSceAppInstUtil itself (Playden found the same), so
// it writes a request, sends tools/title-remover to the payload loader on
// 127.0.0.1:9021, reads the payload's answer, then waits for the title to
// leave app.db (the uninstall finishes asynchronously).
// docs/plans/collect-delete.md, section 4.
struct TitleRemoverConfig
{
    std::string request_dir = "/data/prospero-disk/title-remover";
    std::string payload; // the ELF, in the install folder
    // Sends the ELF's bytes to the payload loader; false with a reason.
    std::function<bool(const std::string &elf, std::string *error)> send;
    // Whether the console still lists the title (app.db).
    std::function<bool(const std::string &title_id)> listed;
    std::function<void(int milliseconds)> sleep;
    int answer_polls = 60; // x answer_ms: how long the payload may take to answer
    int answer_ms = 250;
    int confirm_polls = 60; // x confirm_ms: how long the system may take to finish
    int confirm_ms = 1000;
};

class PayloadTitleRemover final : public TitleRemover
{
  public:
    explicit PayloadTitleRemover(TitleRemoverConfig config) : config_(std::move(config))
    {
    }
    UninstallResult uninstall(const std::string &title_id) override;

  private:
    TitleRemoverConfig config_;
    unsigned serial_ = 0;
};

// Streams an ELF to the payload loader on 127.0.0.1:9021.
bool send_to_payload_loader(const std::string &elf, std::string *error);

// The fixture's uninstall: removes <root>/app/<id>, <root>/patch/<id> and
// <root>/appmeta/<id> from the fixture file system, as the console would.
std::unique_ptr<TitleRemover> make_fixture_title_remover(const FileSystem &fs, std::string root);

} // namespace hui::disk
