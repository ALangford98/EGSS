#!/usr/bin/env bash
# Linux counterpart to BuildProject.bat -- generates GNU makefiles instead of
# a Visual Studio solution.
set -e
cd "$(dirname "$0")"
vendor/bin/premake/premake5 gmake2
echo "Generated makefiles. Build with:  make config=debug"
