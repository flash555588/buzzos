#ifndef BUZZOS_APP_IDENTITY_H
#define BUZZOS_APP_IDENTITY_H

#include "uikit.h"

struct ui_app_identity {
    const char *name;
    const char *title;
    const char *description;
    int icon;
    uint32_t color;
};

/* App names are ASCII; keep matching independent of locale and allocation. */
static inline int ui_text_contains_ascii_ci(const char *text, const char *query) {
    if (!query || !*query) return 1;
    if (!text) return 0;
    for (; *text; text++) {
        const char *hay = text, *needle = query;
        while (*hay && *needle) {
            unsigned char a = (unsigned char)*hay, b = (unsigned char)*needle;
            if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
            if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
            if (a != b) break;
            hay++; needle++;
        }
        if (!*needle) return 1;
    }
    return 0;
}

static inline const struct ui_app_identity *ui_app_identity(const char *name) {
    static const struct ui_app_identity apps[] = {
        {"terminal", "Terminal", "Commands and system tools", UI_ICON_TERMINAL, 0xF3AD87u},
        {"taskmanager", "Task Manager", "Processes and performance", UI_ICON_CHART, 0xB9DCA3u},
        {"textedit", "Text Editor", "Notes and documents", UI_ICON_DOCUMENT, 0xA7C6F2u},
        {"paint", "Paint", "Create something colorful", UI_ICON_IMAGE, 0xD0B5EBu},
        {"calculator", "Calculator", "Everyday calculations", UI_ICON_CALCULATOR, 0xF5D46Fu},
        {"filemanager", "Files", "Browse and organize your files", UI_ICON_FOLDER, 0xF1BD7Au},
        {"browser", "Browser", "Explore the web", UI_ICON_GLOBE, 0xA5D6CAu},
        {"doom", "DOOM", "Classic first-person adventure", UI_ICON_GAMEPAD, 0xF0A99Du},
        {"music", "Music", "Your music, beautifully played", UI_ICON_MUSIC, 0xECB5CFu},
        {"gameboy", "Game Boy", "A pocket-sized classic", UI_ICON_GAMEPAD, 0xBDB9EEu},
        {"luaide", "Lua Studio", "Write, run and explore", UI_ICON_CODE, 0xBCD3A0u},
        {"apps", "Workspace", "Everything in one place", UI_ICON_GRID, 0xFFD15Cu},
        {"settings", "Settings", "Make the desktop yours", UI_ICON_SETTINGS, 0xD4CBBCu},
    };
    static const struct ui_app_identity fallback = {
        0, 0, "Desktop application", UI_ICON_GRID, 0x607FA9u
    };
    for (unsigned int i = 0; i < sizeof(apps) / sizeof(apps[0]); i++)
        if (name && strcmp(name, apps[i].name) == 0)
            return &apps[i];
    return &fallback;
}

static inline const char *ui_app_title(const char *name) {
    const struct ui_app_identity *app = ui_app_identity(name);
    return app->title ? app->title : (name ? name : "Application");
}

static inline void ui_app_badge(struct ui_surface *s, const char *name,
                                int x, int y, int side) {
    const struct ui_app_identity *app = ui_app_identity(name);
    struct ui_rect box = ui_rect_make(x, y, side, side);
    ui_fill_round(s, box, UI_RADIUS_CONTROL, app->color);
    ui_stroke_round(s, box, UI_RADIUS_CONTROL, 1, UI_TEXT_PRIMARY, 60);
    ui_icon_in(s, app->icon, box, side * 3 / 5, UI_TEXT_PRIMARY, 255);
}

#endif
