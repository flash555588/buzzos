#!/usr/bin/env python3
import argparse
import re
from pathlib import Path
from string import Template

ROOT = Path(__file__).resolve().parents[1]


C_TEMPLATE = Template(r'''#include "appui.h"
#include "guiapp.h"
#include "libc.h"

enum { W = 420, H = 260 };

static uint8_t pixels[W * H];
static int click_count;
static int prev_buttons;

static void load_state(void) {
    char buf[16];
    int fd = open("/fs/apps/$name.cfg", O_RDONLY);
    if (fd < 0)
        return;
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n > 0) {
        buf[n] = 0;
        click_count = atoi(buf);
    }
}

static void save_state(void) {
    char buf[16] = "";
    appui_append_int(buf, click_count, sizeof(buf));
    int fd = open("/fs/apps/$name.cfg", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return;
    write(fd, buf, strlen(buf));
    close(fd);
}

static struct appui_rect action_button(void) {
    return (struct appui_rect){24, 126, 150, 38};
}

static void render(void) {
    char count[48] = "Actions: ";
    appui_append_int(count, click_count, sizeof(count));
    appui_fill(pixels, W, H, (struct appui_rect){0, 0, W, H}, appui_gray(3));
    appui_fill(pixels, W, H, (struct appui_rect){16, 16, W - 32, H - 32}, appui_gray(5));
    appui_border(pixels, W, H, (struct appui_rect){16, 16, W - 32, H - 32},
                 appui_gray(9), appui_gray(1));
    appui_text(pixels, W, H, 30, 36, "$upper", 15, -1,
               (struct appui_rect){24, 24, W - 48, 28});
    appui_text(pixels, W, H, 30, 78, "Generated BuzzOS desktop app", 15, -1,
               (struct appui_rect){24, 66, W - 48, 32});
    appui_button(pixels, W, H, action_button(), "Action", 1);
    appui_text(pixels, W, H, 30, 190, count, 15, -1,
               (struct appui_rect){24, 180, W - 48, 32});
}

static void activate(void) {
    click_count++;
    save_state();
}

static void handle_mouse(int x, int y, int buttons) {
    int pressed = (buttons & 1) && !(prev_buttons & 1);
    prev_buttons = buttons;
    if (pressed && appui_inside(x, y, action_button()))
        activate();
}

static void handle_key(int key) {
    if (key == '\n' || key == '\r' || key == ' ')
        activate();
}

int main(int argc, char **argv) {
    struct guiapp_ctx ctx;
    struct guiapp_event ev;
    if (guiapp_parse_args(argc, argv, &ctx) < 0)
        return 1;
    load_state();
    for (;;) {
        if (guiapp_read_event(&ctx, &ev) < 0 || ev.type == GUIAPP_EVT_CLOSE)
            break;
        if (ev.type == GUIAPP_EVT_MOUSE)
            handle_mouse(ev.x, ev.y, ev.buttons);
        else if (ev.type == GUIAPP_EVT_KEY)
            handle_key(ev.key);
        render();
        if (guiapp_send_frame(&ctx, "$upper", W, H, pixels) < 0)
            break;
    }
    return 0;
}
''')


def clean_single_line(value):
    return " ".join(value.split())


def validate_name(name):
    if not re.fullmatch(r"[a-z][a-z0-9_]{0,22}", name):
        raise SystemExit("app name must match [a-z][a-z0-9_]{0,22}")
    return name


def write_file(path, content, force):
    if path.exists() and not force:
        raise SystemExit(f"{path} already exists; use --force to overwrite")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")


def render_manifest(name, summary):
    return (
        f"name={name.upper()}\n"
        "kind=gui\n"
        "version=0.1\n"
        f"summary={summary}\n"
        f"exec=/fs/apps/{name}\n"
        f"state=/fs/apps/{name}.cfg\n"
        f"source=src/user/bin/{name}.c\n"
        f"readme=/fs/apps/{name}.readme\n"
    )


def render_readme(name, summary):
    return (
        f"BuzzOS {name.upper()} desktop app\n\n"
        f"{summary}\n\n"
        f"Executable: /fs/apps/{name}\n"
        f"Run: apps run {name}\n"
        f"State: /fs/apps/{name}.cfg\n"
    )


def print_next_steps(name):
    print("\nNext steps:")
    print(f"1. Add `{name}` to GUI_APP_NAMES in Makefile.")
    print("2. Optionally add default state to `src/user/bin/" + name + ".seed`.")
    print("3. Run `make app-registry`, `make app-check`, then `make verify`.")


def main():
    parser = argparse.ArgumentParser(description="Create a BuzzOS desktop app scaffold")
    parser.add_argument("name", help="lowercase app name, for example todo")
    parser.add_argument("--summary", default="Generated desktop app", help="manifest summary")
    parser.add_argument("--force", action="store_true", help="overwrite existing scaffold files")
    parser.add_argument("--dry-run", action="store_true", help="show paths without writing files")
    args = parser.parse_args()

    name = validate_name(args.name)
    summary = clean_single_line(args.summary)
    files = {
        ROOT / f"src/user/bin/{name}.c": C_TEMPLATE.substitute(name=name, upper=name.upper()),
        ROOT / f"src/user/bin/{name}.app": render_manifest(name, summary),
        ROOT / f"src/user/bin/{name}.readme": render_readme(name, summary),
    }
    for path in files:
        print(("would create " if args.dry_run else "create ") + str(path.relative_to(ROOT)))
    if not args.dry_run:
        for path, content in files.items():
            write_file(path, content, args.force)
    print_next_steps(name)


if __name__ == "__main__":
    main()
