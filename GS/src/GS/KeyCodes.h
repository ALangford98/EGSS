#pragma once

// Values match GLFW's, so the platform layer is a straight pass-through.
// Client code should use these rather than including GLFW headers.

#define GS_KEY_SPACE              32
#define GS_KEY_APOSTROPHE         39  /* ' */
#define GS_KEY_COMMA              44  /* , */
#define GS_KEY_MINUS              45  /* - */
#define GS_KEY_PERIOD             46  /* . */
#define GS_KEY_SLASH              47  /* / */
#define GS_KEY_0                  48
#define GS_KEY_1                  49
#define GS_KEY_2                  50
#define GS_KEY_3                  51
#define GS_KEY_4                  52
#define GS_KEY_5                  53
#define GS_KEY_6                  54
#define GS_KEY_7                  55
#define GS_KEY_8                  56
#define GS_KEY_9                  57
#define GS_KEY_SEMICOLON          59  /* ; */
#define GS_KEY_EQUAL              61  /* = */
#define GS_KEY_A                  65
#define GS_KEY_B                  66
#define GS_KEY_C                  67
#define GS_KEY_D                  68
#define GS_KEY_E                  69
#define GS_KEY_F                  70
#define GS_KEY_G                  71
#define GS_KEY_H                  72
#define GS_KEY_I                  73
#define GS_KEY_J                  74
#define GS_KEY_K                  75
#define GS_KEY_L                  76
#define GS_KEY_M                  77
#define GS_KEY_N                  78
#define GS_KEY_O                  79
#define GS_KEY_P                  80
#define GS_KEY_Q                  81
#define GS_KEY_R                  82
#define GS_KEY_S                  83
#define GS_KEY_T                  84
#define GS_KEY_U                  85
#define GS_KEY_V                  86
#define GS_KEY_W                  87
#define GS_KEY_X                  88
#define GS_KEY_Y                  89
#define GS_KEY_Z                  90
#define GS_KEY_LEFT_BRACKET       91  /* [ */
#define GS_KEY_BACKSLASH          92  /* \ */
#define GS_KEY_RIGHT_BRACKET      93  /* ] */
#define GS_KEY_GRAVE_ACCENT       96  /* ` */

/* Function keys */
#define GS_KEY_ESCAPE             256
#define GS_KEY_ENTER              257
#define GS_KEY_TAB                258
#define GS_KEY_BACKSPACE          259
#define GS_KEY_INSERT             260
#define GS_KEY_DELETE             261
#define GS_KEY_RIGHT              262
#define GS_KEY_LEFT               263
#define GS_KEY_DOWN               264
#define GS_KEY_UP                 265
#define GS_KEY_PAGE_UP            266
#define GS_KEY_PAGE_DOWN          267
#define GS_KEY_HOME               268
#define GS_KEY_END                269
#define GS_KEY_CAPS_LOCK          280
#define GS_KEY_SCROLL_LOCK        281
#define GS_KEY_NUM_LOCK           282
#define GS_KEY_PRINT_SCREEN       283
#define GS_KEY_PAUSE              284
#define GS_KEY_F1                 290
#define GS_KEY_F2                 291
#define GS_KEY_F3                 292
#define GS_KEY_F4                 293
#define GS_KEY_F5                 294
#define GS_KEY_F6                 295
#define GS_KEY_F7                 296
#define GS_KEY_F8                 297
#define GS_KEY_F9                 298
#define GS_KEY_F10                299
#define GS_KEY_F11                300
#define GS_KEY_F12                301

/* Keypad */
#define GS_KEY_KP_0               320
#define GS_KEY_KP_1               321
#define GS_KEY_KP_2               322
#define GS_KEY_KP_3               323
#define GS_KEY_KP_4               324
#define GS_KEY_KP_5               325
#define GS_KEY_KP_6               326
#define GS_KEY_KP_7               327
#define GS_KEY_KP_8               328
#define GS_KEY_KP_9               329
#define GS_KEY_KP_DECIMAL         330
#define GS_KEY_KP_DIVIDE          331
#define GS_KEY_KP_MULTIPLY        332
#define GS_KEY_KP_SUBTRACT        333
#define GS_KEY_KP_ADD             334
#define GS_KEY_KP_ENTER           335
#define GS_KEY_KP_EQUAL           336

/* Modifiers */
#define GS_KEY_LEFT_SHIFT         340
#define GS_KEY_LEFT_CONTROL       341
#define GS_KEY_LEFT_ALT           342
#define GS_KEY_LEFT_SUPER         343
#define GS_KEY_RIGHT_SHIFT        344
#define GS_KEY_RIGHT_CONTROL      345
#define GS_KEY_RIGHT_ALT          346
#define GS_KEY_RIGHT_SUPER        347
#define GS_KEY_MENU               348
