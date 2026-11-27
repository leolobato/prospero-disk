// Prospero Disk - The one-shot title remover payload: uninstalls one title through the system.
// Copyright (C) 2026 Leonardo Lobato and Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Adapted from Playden's tools/title-remover. Prospero Disk (PPSA93475), like
// Playden, cannot load libSceAppInstUtil itself. It sends this payload to the
// payload loader on 127.0.0.1:9021; the payload uninstalls one title and
// exits. It reads one request from REQUEST_DIR:
//
//   request   "title=PPSA01467\nnonce=<16..64 of [0-9a-f-]>\n"
//
// The folder and the request must belong to the same user, who is not root
// (the app writes them as its own user; /data/prospero-disk itself is
// root's), must not be links, and the request has one link. The title must
// be a PS5 or PS4 game (PPSA or CUSA and five digits) or one of Playden's
// game titles (FAKE78 and three digits), never Prospero Disk itself. The answer goes next
// to the request, written as that user:
//
//   result    "title=<id>\nnonce=<nonce>\ncode=<8 hex digits>\n"   (code 0 = accepted)
//
// The uninstall finishes asynchronously; the app confirms it in app.db.
// Order matters (Playden, results/20261007-native-uninstall): initialize
// AppInstUtil, then take the installer's authid (as ShadowMountPlus does),
// then uninstall.
#include <ps5/kernel.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define REQUEST_DIR "/data/prospero-disk/title-remover"
#define SELF_TITLE "PPSA93475"
#define AUTHID_INSTALLER 0x4800000000000006ull

int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppUnInstall(const char *title_id);

static int digits(const char *text, size_t count) {
  for (size_t i = 0; i < count; ++i)
    if (text[i] < '0' || text[i] > '9') return 0;
  return 1;
}

static int valid_title(const char *title) {
  if (strlen(title) != 9 || strcmp(title, SELF_TITLE) == 0) return 0;
  if (strncmp(title, "PPSA", 4) == 0 || strncmp(title, "CUSA", 4) == 0) return digits(title + 4, 5);
  if (strncmp(title, "FAKE78", 6) == 0) return digits(title + 6, 3);
  return 0;
}

static int valid_nonce(const char *nonce) {
  const size_t length = strlen(nonce);
  if (length < 16 || length > 64) return 0;
  for (size_t i = 0; i < length; ++i)
    if (!((nonce[i] >= '0' && nonce[i] <= '9') || (nonce[i] >= 'a' && nonce[i] <= 'f') || nonce[i] == '-')) return 0;
  return 1;
}

// Reads "key=value" from the request text into value (bounded).
static int field(const char *text, const char *key, char *value, size_t size) {
  const size_t key_length = strlen(key);
  for (const char *line = text; *line;) {
    const char *end = strchr(line, '\n');
    const size_t length = end ? (size_t)(end - line) : strlen(line);
    if (length > key_length && strncmp(line, key, key_length) == 0 && line[key_length] == '=') {
      const size_t value_length = length - key_length - 1;
      if (value_length >= size) return 0;
      memcpy(value, line + key_length + 1, value_length);
      value[value_length] = 0;
      return 1;
    }
    if (!end) break;
    line = end + 1;
  }
  return 0;
}

static void write_result(uid_t owner, const char *title, const char *nonce, unsigned code) {
  // As the app's user, so the answer sits in its folder like the request.
  kernel_set_ucred_uid(-1, owner);
  char text[160];
  const int length = snprintf(text, sizeof text, "title=%s\nnonce=%s\ncode=%08x\n", title, nonce, code);
  // A file of our own, made here: never one someone left in the folder.
  unlink(REQUEST_DIR "/result.tmp");
  const int fd = open(REQUEST_DIR "/result.tmp", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0666);
  if (fd < 0) return;
  fchmod(fd, 0666);
  const int written = (int)write(fd, text, (size_t)length) == length;
  fsync(fd);
  close(fd);
  if (written) rename(REQUEST_DIR "/result.tmp", REQUEST_DIR "/result");
  else unlink(REQUEST_DIR "/result.tmp");
}

int main(void) {
  struct stat folder;
  if (lstat(REQUEST_DIR, &folder) != 0 || !S_ISDIR(folder.st_mode) || folder.st_uid == 0) return 1;
  const uid_t owner = folder.st_uid;
  // The checks are on the file that was opened, not on the name before it.
  char text[256] = {0};
  const int fd = open(REQUEST_DIR "/request", O_RDONLY | O_NOFOLLOW);
  if (fd < 0) return 3;
  struct stat request;
  if (fstat(fd, &request) != 0 || !S_ISREG(request.st_mode) || request.st_uid != owner ||
      request.st_nlink != 1 || request.st_size <= 0 || request.st_size >= 256) {
    close(fd);
    return 2;
  }
  const ssize_t count = read(fd, text, sizeof text - 1);
  close(fd);
  if (count <= 0) return 3;
  char title[16], nonce[72];
  if (!field(text, "title", title, sizeof title) || !field(text, "nonce", nonce, sizeof nonce) ||
      !valid_title(title) || !valid_nonce(nonce))
    return 4;
  unsigned code = (unsigned)sceAppInstUtilInitialize();
  if (code == 0) {
    kernel_set_ucred_authid(-1, AUTHID_INSTALLER);
    code = (unsigned)sceAppInstUtilAppUnInstall(title);
  }
  write_result(owner, title, nonce, code);
  return code == 0 ? 0 : 5;
}
