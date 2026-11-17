// ps5-homebrew-ui - Factories of every UI design in the app.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "app/concept.hpp"

#include <memory>

namespace hui::concepts
{

// One function per design, each defined in its own file in this directory.
// To add a design: write the file, declare its factory here and list it in
// registry.cpp. Nothing else in the app needs to change.
std::unique_ptr<app::Concept> make_disk(app::Context &context);
// The kit's Component Library, kept as a reference while the app is built.
std::unique_ptr<app::Concept> make_components(app::Context &context);

} // namespace hui::concepts
