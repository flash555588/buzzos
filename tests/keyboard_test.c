#include <stdio.h>
#include <string.h>
#include "keyboard.h"
#include "../src/user/libc/guiapp.h"

_Static_assert(KEYBOARD_ZOOM_IN == (int)GUIAPP_KEY_ZOOM_IN, "zoom-in protocol mismatch");
_Static_assert(KEYBOARD_ZOOM_OUT == (int)GUIAPP_KEY_ZOOM_OUT, "zoom-out protocol mismatch");
_Static_assert(KEYBOARD_ZOOM_RESET == (int)GUIAPP_KEY_ZOOM_RESET, "zoom-reset protocol mismatch");

static int failures;
void gui_event_notify_display(void) {}

static void expect(const char *sequence) {
    for (const unsigned char *p = (const unsigned char *)sequence; *p; p++) {
        int actual = keyboard_getchar();
        if (actual != *p) {
            fprintf(stderr, "keyboard: expected %u, got %d\n", *p, actual);
            failures++;
        }
    }
    if (keyboard_getchar() != -1) failures++;
}

static void tap(unsigned char code, int extended) {
    if (extended) keyboard_handler(0xE0);
    keyboard_handler(code);
    if (extended) keyboard_handler(0xE0);
    keyboard_handler(code | 0x80);
}

int main(void) {
    keyboard_init();
    tap(0x1E, 0);
    expect("a");
    keyboard_handler(0x1D);
    tap(0x19, 0);
    expect("\x10");
    tap(0x39, 0);
    expect("\x1F");
    keyboard_handler(0x9D);
    tap(0x48, 1);
    expect("\x1B[A");
    tap(0x53, 1);
    expect("\x1B[3~");
    while (keyboard_getevent() >= 0) {}
    keyboard_handler(0x38);
    const unsigned char codes[] = {0x0F, 0x20, 0x3E, 0x48, 0x50, 0x4D, 0x4B};
    const char *sequences[] = {"\x1B[T", "\x1B[W", "\x1B[Q", "\x1B[a",
                               "\x1B[b", "\x1B[c", "\x1B[d"};
    for (int i = 0; i < 7; i++) {
        tap(codes[i], i >= 3);
        expect(sequences[i]);
        if (keyboard_getevent() != -1) failures++;
    }
    keyboard_handler(0xB8);
    tap(0x20, 0);
    expect("d");
    tap(0x4B, 1);
    expect("\x1B[D");
    keyboard_handler(0x38);
    keyboard_init();
    tap(0x20, 0);
    expect("d");
    while (keyboard_getevent() >= 0) {}
    keyboard_handler(0x2A);
    keyboard_handler(0x2D); /* Shift+X */
    expect("X");
    if (keyboard_getevent() != (0x8000 | 'X')) failures++;
    keyboard_handler(0xAA); /* Release Shift before X. */
    keyboard_handler(0xAD);
    if (keyboard_getevent() != 'X') failures++;
    keyboard_handler(0xE0);
    keyboard_handler(0x4D); /* Hold Right, then press Alt. */
    expect("\x1B[C");
    if (keyboard_getevent() != (0x8000 | 258)) failures++;
    keyboard_handler(0x38);
    keyboard_handler(0xE0);
    keyboard_handler(0xCD);
    if (keyboard_getevent() != 258) failures++;
    keyboard_handler(0xB8);
    keyboard_handler(0x1D);
    keyboard_handler(0x2E);
    expect("\x03");
    if (keyboard_getevent() != (0x8000 | 3)) failures++;
    keyboard_handler(0x9D);
    keyboard_handler(0xAE);
    if (keyboard_getevent() != 3) failures++;
    keyboard_init();
    keyboard_handler(0x1D);
    const unsigned char zoom_codes[] = {0x0D, 0x0C, 0x0B, 0x4E, 0x4A, 0x52};
    const char *zoom_sequences[] = {"\x1B[+", "\x1B[-", "\x1B[0",
                                    "\x1B[+", "\x1B[-", "\x1B[0"};
    const int zoom_keys[] = {KEYBOARD_ZOOM_IN, KEYBOARD_ZOOM_OUT, KEYBOARD_ZOOM_RESET,
                            KEYBOARD_ZOOM_IN, KEYBOARD_ZOOM_OUT, KEYBOARD_ZOOM_RESET};
    for (int index = 0; index < 6; index++) {
        tap(zoom_codes[index], 0);
        expect(zoom_sequences[index]);
        if (keyboard_getevent() != (0x8000 | zoom_keys[index])) failures++;
        if (keyboard_getevent() != zoom_keys[index]) failures++;
    }
    keyboard_handler(0x2A);
    keyboard_handler(0x0D);
    expect("\x1B[+");
    if (keyboard_getevent() != (0x8000 | KEYBOARD_ZOOM_IN)) failures++;
    keyboard_handler(0xAA);
    keyboard_handler(0x9D);
    keyboard_handler(0x8D);
    if (keyboard_getevent() != KEYBOARD_ZOOM_IN) failures++;
    tap(0x0D, 0);
    expect("=");
    tap(0x0C, 0);
    expect("-");
    tap(0x0B, 0);
    expect("0");
    keyboard_handler(0x2A);
    tap(0x0D, 0);
    expect("+");
    puts(failures ? "keyboard tests FAILED" : "keyboard tests passed");
    return failures != 0;
}
