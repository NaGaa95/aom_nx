/* imports.h -- .so import resolution for Adventure of Mana
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __IMPORTS_H__
#define __IMPORTS_H__

#include "so_util.h"

// Relocate + resolve both modules. libc++_shared must already be loaded so the
// engine's std::string / RTTI / operator-new imports resolve from it.
void aom_resolve_imports(so_module *cxx_mod, so_module *game_mod);

#endif
