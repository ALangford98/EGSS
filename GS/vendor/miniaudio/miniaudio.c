/* The single translation unit that compiles miniaudio itself.
   Kept as .c so it builds with the C compiler, which is what miniaudio
   expects; the engine only ever sees the header. */
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
