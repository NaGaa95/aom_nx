/* save_path.c -- keep Adventure of Mana saves beside the NRO
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <switch.h>

#include "save_path.h"
#include "util.h"

#define ENGINE_SAVE_PREFIX "/data/data/" SAVE_PACKAGE_NAME "/files"

const char *save_redirect_path(const char *path, char *redirected, size_t size) {
  if (!path || !redirected || size == 0)
    return path;

  const size_t prefix_len = sizeof(ENGINE_SAVE_PREFIX) - 1;
  if (strncmp(path, ENGINE_SAVE_PREFIX, prefix_len) != 0 ||
      (path[prefix_len] != 0 && path[prefix_len] != '/'))
    return path;

  const char *name = path + prefix_len;
  while (*name == '/')
    name++;

  if (*name)
    snprintf(redirected, size, "./%s", name);
  else
    snprintf(redirected, size, ".");
  return redirected;
}

static int is_game_save(const char *name) {
  const size_t len = name ? strlen(name) : 0;
  return len > 8 && strncmp(name, "sk1_", 4) == 0 &&
         strcmp(name + len - 4, ".sav") == 0;
}

static int files_equal(const char *left_path, const char *right_path) {
  unsigned char left[16384], right[16384];
  FILE *a = fopen(left_path, "rb");
  FILE *b = fopen(right_path, "rb");
  if (!a || !b) {
    if (a) fclose(a);
    if (b) fclose(b);
    return 0;
  }

  int equal = 1;
  for (;;) {
    const size_t na = fread(left, 1, sizeof(left), a);
    const size_t nb = fread(right, 1, sizeof(right), b);
    if (na != nb || memcmp(left, right, na) != 0) {
      equal = 0;
      break;
    }
    if (na < sizeof(left)) {
      if (ferror(a) || ferror(b))
        equal = 0;
      break;
    }
  }

  fclose(a);
  fclose(b);
  return equal;
}

/* rename() is atomic on the SD card.  The copy fallback handles any unusual
 * cross-device path setup and deletes the source only after byte verification. */
static int move_save(const char *source, const char *destination) {
  struct stat st;
  if (stat(destination, &st) == 0)
    return 0; /* Never replace a save already made in the new location. */

  if (rename(source, destination) == 0)
    return 1;

  char temporary[512];
  const int len = snprintf(temporary, sizeof(temporary), "%s.migrating", destination);
  if (len <= 0 || len >= (int)sizeof(temporary))
    return -1;

  FILE *in = fopen(source, "rb");
  FILE *out = fopen(temporary, "wb");
  if (!in || !out) {
    if (in) fclose(in);
    if (out) fclose(out);
    remove(temporary);
    return -1;
  }

  unsigned char data[32768];
  int ok = 1;
  size_t count;
  while ((count = fread(data, 1, sizeof(data), in)) != 0) {
    if (fwrite(data, 1, count, out) != count) {
      ok = 0;
      break;
    }
  }
  if (ferror(in) || fflush(out) != 0)
    ok = 0;
  fclose(in);
  if (fclose(out) != 0)
    ok = 0;

  if (!ok || !files_equal(source, temporary) || rename(temporary, destination) != 0) {
    remove(temporary);
    return -1;
  }

  if (remove(source) != 0)
    debugPrintf("save migration: copied but could not remove %s\n", source);
  return 1;
}

static void migrate_directory(const char *directory) {
  DIR *dir = opendir(directory);
  if (!dir)
    return;

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (!is_game_save(entry->d_name))
      continue;

    char source[512], destination[512];
    const int source_len = snprintf(source, sizeof(source), "%s/%s", directory, entry->d_name);
    const int destination_len = snprintf(destination, sizeof(destination), "./%s", entry->d_name);
    if (source_len <= 0 || source_len >= (int)sizeof(source) ||
        destination_len <= 0 || destination_len >= (int)sizeof(destination))
      continue;

    struct stat st;
    if (stat(source, &st) != 0 || !S_ISREG(st.st_mode))
      continue;

    const int result = move_save(source, destination);
    if (result > 0)
      debugPrintf("save migration: moved %s to the game folder\n", entry->d_name);
    else if (result < 0)
      debugPrintf("save migration: failed to move %s\n", source);
  }

  closedir(dir);
  rmdir(directory); /* Succeeds only when no files belonging to anything else remain. */
}

void migrate_legacy_saves(void) {
  /* The first path is where existing releases actually wrote saves.  The
   * package paths cover the documented/intended Android-compatible location. */
  static const char *const legacy_directories[] = {
    "/data/data/files",
    "/data/data/com.square_enix.adventures/files",
    "/data/data/com.square_enix.adventures",
  };

  for (size_t i = 0; i < sizeof(legacy_directories) / sizeof(*legacy_directories); i++)
    migrate_directory(legacy_directories[i]);

  rmdir("/data/data/com.square_enix.adventures");
}
