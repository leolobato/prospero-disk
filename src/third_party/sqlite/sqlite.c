// Prospero Disk - The single compilation unit for SQLite (tools/fetch-sqlite.sh).
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SQLite is public domain and is not part of this repository: the build fetches
// its pinned amalgamation into .deps/sqlite. The app only reads the console's
// app.db, from one thread, so the build leaves out threads, extensions, the
// shared cache and the query planner's extras.

#define SQLITE_THREADSAFE 0
#define SQLITE_OMIT_LOAD_EXTENSION 1
#define SQLITE_OMIT_SHARED_CACHE 1
#define SQLITE_OMIT_DEPRECATED 1
#define SQLITE_DEFAULT_MEMSTATUS 0
#define SQLITE_DQS 0
#include "../../../.deps/sqlite/sqlite3.c"
