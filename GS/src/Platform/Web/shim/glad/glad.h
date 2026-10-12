#pragma once
// Stands in for glad on the web build (export piece 5). In a browser the GL
// entry points are WebGL2's, provided by Emscripten's GLES3 headers and bound
// at link time -- there is nothing to load at run time, so the loader is a
// no-op and the engine's existing `#include <glad/glad.h>` keeps working.
// Only on the include path when building with --web.
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

typedef void* (*GLADloadproc)(const char* name);
inline int gladLoadGLLoader(GLADloadproc) { return 1; }
