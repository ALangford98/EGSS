#pragma once

// Values match GLFW's, so the platform layer is a straight pass-through.
// Client code should use these rather than including GLFW headers.

#define EGSS_KEY_SPACE              32
#define EGSS_KEY_APOSTROPHE         39  /* ' */
#define EGSS_KEY_COMMA              44  /* , */
#define EGSS_KEY_MINUS              45  /* - */
#define EGSS_KEY_PERIOD             46  /* . */
#define EGSS_KEY_SLASH              47  /* / */
#define EGSS_KEY_0                  48
#define EGSS_KEY_1                  49
#define EGSS_KEY_2                  50
#define EGSS_KEY_3                  51
#define EGSS_KEY_4                  52
#define EGSS_KEY_5                  53
#define EGSS_KEY_6                  54
#define EGSS_KEY_7                  55
#define EGSS_KEY_8                  56
#define EGSS_KEY_9                  57
#define EGSS_KEY_SEMICOLON          59  /* ; */
#define EGSS_KEY_EQUAL              61  /* = */
#define EGSS_KEY_A                  65
#define EGSS_KEY_B                  66
#define EGSS_KEY_C                  67
#define EGSS_KEY_D                  68
#define EGSS_KEY_E                  69
#define EGSS_KEY_F                  70
#define EGSS_KEY_G                  71
#define EGSS_KEY_H                  72
#define EGSS_KEY_I                  73
#define EGSS_KEY_J                  74
#define EGSS_KEY_K                  75
#define EGSS_KEY_L                  76
#define EGSS_KEY_M                  77
#define EGSS_KEY_N                  78
#define EGSS_KEY_O                  79
#define EGSS_KEY_P                  80
#define EGSS_KEY_Q                  81
#define EGSS_KEY_R                  82
#define EGSS_KEY_S                  83
#define EGSS_KEY_T                  84
#define EGSS_KEY_U                  85
#define EGSS_KEY_V                  86
#define EGSS_KEY_W                  87
#define EGSS_KEY_X                  88
#define EGSS_KEY_Y                  89
#define EGSS_KEY_Z                  90
#define EGSS_KEY_LEFT_BRACKET       91  /* [ */
#define EGSS_KEY_BACKSLASH          92  /* \ */
#define EGSS_KEY_RIGHT_BRACKET      93  /* ] */
#define EGSS_KEY_GRAVE_ACCENT       96  /* ` */

/* Function keys */
#define EGSS_KEY_ESCAPE             256
#define EGSS_KEY_ENTER              257
#define EGSS_KEY_TAB                258
#define EGSS_KEY_BACKSPACE          259
#define EGSS_KEY_INSERT             260
#define EGSS_KEY_DELETE             261
#define EGSS_KEY_RIGHT              262
#define EGSS_KEY_LEFT               263
#define EGSS_KEY_DOWN               264
#define EGSS_KEY_UP                 265
#define EGSS_KEY_PAGE_UP            266
#define EGSS_KEY_PAGE_DOWN          267
#define EGSS_KEY_HOME               268
#define EGSS_KEY_END                269
#define EGSS_KEY_CAPS_LOCK          280
#define EGSS_KEY_SCROLL_LOCK        281
#define EGSS_KEY_NUM_LOCK           282
#define EGSS_KEY_PRINT_SCREEN       283
#define EGSS_KEY_PAUSE              284
#define EGSS_KEY_F1                 290
#define EGSS_KEY_F2                 291
#define EGSS_KEY_F3                 292
#define EGSS_KEY_F4                 293
#define EGSS_KEY_F5                 294
#define EGSS_KEY_F6                 295
#define EGSS_KEY_F7                 296
#define EGSS_KEY_F8                 297
#define EGSS_KEY_F9                 298
#define EGSS_KEY_F10                299
#define EGSS_KEY_F11                300
#define EGSS_KEY_F12                301

/* Keypad */
#define EGSS_KEY_KP_0               320
#define EGSS_KEY_KP_1               321
#define EGSS_KEY_KP_2               322
#define EGSS_KEY_KP_3               323
#define EGSS_KEY_KP_4               324
#define EGSS_KEY_KP_5               325
#define EGSS_KEY_KP_6               326
#define EGSS_KEY_KP_7               327
#define EGSS_KEY_KP_8               328
#define EGSS_KEY_KP_9               329
#define EGSS_KEY_KP_DECIMAL         330
#define EGSS_KEY_KP_DIVIDE          331
#define EGSS_KEY_KP_MULTIPLY        332
#define EGSS_KEY_KP_SUBTRACT        333
#define EGSS_KEY_KP_ADD             334
#define EGSS_KEY_KP_ENTER           335
#define EGSS_KEY_KP_EQUAL           336

/* Modifiers */
#define EGSS_KEY_LEFT_SHIFT         340
#define EGSS_KEY_LEFT_CONTROL       341
#define EGSS_KEY_LEFT_ALT           342
#define EGSS_KEY_LEFT_SUPER         343
#define EGSS_KEY_RIGHT_SHIFT        344
#define EGSS_KEY_RIGHT_CONTROL      345
#define EGSS_KEY_RIGHT_ALT          346
#define EGSS_KEY_RIGHT_SUPER        347
#define EGSS_KEY_MENU               348
