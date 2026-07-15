/* save_path.h -- save location and legacy-save migration
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __SAVE_PATH_H__
#define __SAVE_PATH_H__

#include <stddef.h>

/* JniConstruct expects an Android package name.  The resulting private-data
 * path is intercepted by the filesystem shim and redirected beside the NRO. */
#define SAVE_PACKAGE_NAME "aom_nx"

const char *save_redirect_path(const char *path, char *redirected, size_t size);
void migrate_legacy_saves(void);

#endif
