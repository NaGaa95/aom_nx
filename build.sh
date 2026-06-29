#!/usr/bin/env bash
# Convenience build script for the Adventure of Mana Switch port.
# Requires devkitA64 + the portlibs listed in README.md.
set -e

# Best run from the MSYS2 / devkitPro shell, where DEVKITPRO=/opt/devkitpro is
# set automatically. This build is verified with devkitA64 gcc 16.1.0.
if [ -z "$DEVKITPRO" ]; then
  if [ -d /opt/devkitpro ]; then
    export DEVKITPRO=/opt/devkitpro
  elif [ -d /c/msys64/opt/devkitpro ]; then
    export DEVKITPRO=/c/msys64/opt/devkitpro
  elif [ -d /c/devkitPro ]; then
    export DEVKITPRO=/c/devkitPro
  else
    echo "error: set DEVKITPRO (e.g. export DEVKITPRO=/opt/devkitpro)"; exit 1
  fi
fi
export DEVKITA64="$DEVKITPRO/devkitA64"
export PATH="$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PATH"

make "$@"
echo
echo "Built: $(ls -1 aom_nx.nro 2>/dev/null || echo '(no nro -- check errors above)')"
