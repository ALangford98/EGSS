#pragma once
// Stands in for glad on the GLES builds -- the web (WebGL2, via Emscripten's
// GLES3 headers) and Android (the NDK's). The GL entry points are bound at
// link time there; there is nothing to load at run time, so the loader is a
// no-op and the engine's existing `#include <glad/glad.h>` keeps working.
// Only on the include path for --web and --android builds.
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

typedef void* (*GLADloadproc)(const char* name);
inline int gladLoadGLLoader(GLADloadproc) { return 1; }
