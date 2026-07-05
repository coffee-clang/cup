$ErrorActionPreference = "Stop"

make PLATFORM=windows-x64 LINK_MODE=dynamic BUILD_MODE=development `
  CC=gcc WINDRES=windres DEPS_PREFIX=/ucrt64
make test-windows
