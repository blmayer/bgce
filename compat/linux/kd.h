/* Minimal linux/kd.h for non-Linux headless builds. */
#ifndef BGCE_COMPAT_LINUX_KD_H
#define BGCE_COMPAT_LINUX_KD_H

#define KDSETMODE  0x4B3A
#define KDGETMODE  0x4B3B
#define KD_TEXT    0x00
#define KD_GRAPHICS 0x01
#define KDGKBMODE  0x4B44
#define KDSKBMODE  0x4B45
#define K_XLATE    0x00
#define K_OFF      0x04

#endif
