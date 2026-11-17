// Prospero Disk - The few SQLite calls the app makes (src/third_party/sqlite/sqlite.c).
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Declared here rather than through sqlite3.h, which lives in the fetched
// .deps/sqlite and is not on every build's include path.

#pragma once

extern "C"
{
    struct sqlite3;
    struct sqlite3_stmt;
    int sqlite3_open_v2(const char *filename, sqlite3 **db, int flags, const char *vfs);
    int sqlite3_close(sqlite3 *db);
    int sqlite3_exec(sqlite3 *db, const char *sql, int (*callback)(void *, int, char **, char **),
                     void *context, char **error);
    int sqlite3_prepare_v2(sqlite3 *db, const char *sql, int bytes, sqlite3_stmt **statement,
                           const char **tail);
    int sqlite3_bind_text(sqlite3_stmt *statement, int index, const char *text, int bytes,
                          void (*destructor)(void *));
    int sqlite3_step(sqlite3_stmt *statement);
    const unsigned char *sqlite3_column_text(sqlite3_stmt *statement, int column);
    int sqlite3_finalize(sqlite3_stmt *statement);
}

namespace hui::disk::sqlite
{

inline constexpr int kOk = 0;
inline constexpr int kRow = 100;
inline constexpr int kOpenReadOnly = 0x00000001;
inline constexpr int kOpenReadWrite = 0x00000002;
inline constexpr int kOpenCreate = 0x00000004;
inline constexpr int kOpenUri = 0x00000040;

} // namespace hui::disk::sqlite
