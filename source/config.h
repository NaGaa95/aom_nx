/* config.h -- Adventure of Mana (SK1) Switch wrapper configuration
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// Engine module + its separately-linked NDK C++ runtime.
#define SO_NAME     "libmcfandroid.so"
#define CXX_SO_NAME "libc++_shared.so"

// Game data tree (sk1/sk1.mpk + sk1patch.mpk + bgm*.ogg), copied next to the NRO.
#define ASSETS_DIR  "assets"

#define CONFIG_NAME "config.txt"
#define LOG_NAME    "debug.log"

#define DEBUG_LOG 0

extern int screen_width;
extern int screen_height;

// The game only ships Japanese and English data
#define LANG_JA 0
#define LANG_EN 1

typedef struct {
  int screen_width;
  int screen_height;
  int language;
  int analog_stick; // 0 = 8-way, 1 = 360° analog
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
