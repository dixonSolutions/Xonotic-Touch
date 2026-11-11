#ifndef TOUCH_ANDROID_H
#define TOUCH_ANDROID_H

#ifdef __ANDROID__
// The Android app in the background: see touch_android.c.
void TouchAndroid_Init(void);              // vid_sdl.c, before SDL_Init
void TouchAndroid_Background(qbool away);  // vid_sdl.c, SDL_APP_DIDENTERBACKGROUND / DIDENTERFOREGROUND
void TouchAndroid_AfterEvents(void);       // vid_sdl.c, after each frame's events
qbool TouchAndroid_HoldMapLoad(void);      // cl_parse.c, before the map's models load
#endif

#endif
