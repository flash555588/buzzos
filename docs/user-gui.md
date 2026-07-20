# BuzzOS User GUI Apps

BuzzOS hosts GUI apps as independent user-space ELF processes. The desktop in
`/bin/gui` owns the framebuffer, window stacking, focus, resizing, minimize,
maximize, close controls, scrollbars, and final composition. Apps receive
events over pipes and return full frames or dirty rectangles.

The build seeds these apps into `/fs/apps`:

```text
/fs/apps/textedit
/fs/apps/textedit.app
/fs/apps/textedit.readme
/fs/apps/paint
/fs/apps/paint.app
/fs/apps/paint.readme
/fs/apps/calculator
/fs/apps/calculator.app
/fs/apps/calculator.readme
```

Current default apps:

| App | Purpose |
| --- | --- |
| TextEdit | Plain text editor. The editing area resizes with the window, supports Enter, cursor movement, horizontal and vertical scrollbars, and saves to `/fs/textedit.txt`. |
| Paint | Bitmap drawing tool. The canvas and toolbar resize with the window, with brush, eraser, line, rectangle, fill, and continuous strokes. |
| Calculator | Expression calculator with decimals, parentheses, and normal arithmetic precedence. |

## Window Behavior

The desktop supports click-to-focus and raise, title-bar dragging, edge and
corner resizing, minimize, maximize, close, mouse wheel scrolling, draggable
scrollbars, and app resize events.

## Run It

From the host, open the desktop directly:

```sh
make run-gui
```

From the text shell:

```text
apps
apps info textedit
apps info paint
apps info calculator
help apps
help gui
help edit
```

From the text shell, start the desktop:

```text
gui
```

Then double-click TextEdit, Paint, or Calculator in the `Applications` window.
The text-shell `apps` command is for manifest inspection; GUI apps are launched
through the desktop.

## Runtime State

The default apps use `/fs` for persistent state:

```text
/fs/textedit.txt
/fs/paint.seed
/fs/calculator.seed
```

TextEdit writes normal text to `/fs/textedit.txt`. Paint and Calculator ship
seed files so the manifest detail panel can show state paths consistently.

## App Manifest

Each app can include a simple `key=value` manifest beside its executable:

```text
/fs/apps/paint
/fs/apps/paint.app
```

Supported manifest keys:

```text
name=Paint
kind=gui
version=1
summary=Bitmap paint app
exec=/fs/apps/paint
state=/fs/paint.seed
source=src/user/bin/paint.c
readme=/fs/apps/paint.readme
```

The App Manager currently uses `name`, `kind`, `version`, `summary`, `state`,
`source`, and `readme` for the detail panel. Unknown keys are ignored, so the
format can grow without breaking older app manifests.

At build time, app metadata lives beside the app source:

```text
src/user/bin/paint.app
src/user/bin/paint.readme
src/user/bin/paint.seed
```

`tools/gen_app_registry.py` turns those sidecar files into
`build/generated/app_registry.h`, which the kernel uses to seed `/fs/apps` at boot.
The generated registry is intentionally checked in like `initrd.h`, making the
boot image reproducible and easy to inspect.

## Create A New App

Create a small GUI app scaffold:

```sh
make new-app APP=todo
```

or preview the files first:

```sh
python tools/new_app.py todo --dry-run
```

The scaffold writes:

```text
src/user/bin/todo.c
src/user/bin/todo.app
src/user/bin/todo.readme
```

The generated C app uses the `guiapp` pipe protocol, draws into an app surface,
handles mouse clicks and keyboard editing, and saves text state under `/fs`.
To make it part of the boot image, add the app name to `GUI_APP_NAMES` in
`Makefile`. Add `src/user/bin/todo.seed` if the app should ship with default
saved state.

Regenerate the kernel app registry:

```sh
make app-registry
```

Validate app packaging without running QEMU:

```sh
make app-check
python tools/check_project.py --list-apps
```

## APIs Used

The sample uses only user-space libc syscall wrappers:

```c
gfx_info(&info);              /* inspect framebuffer size and availability */
gfx_clear(18);
gfx_fill_rect(x, y, w, h, color);
gfx_text(x, y, "TEXT", fg, bg);
fb_blit(x, y, w, h, pixels);
mouse_get(&mouse);
read(0, &key, 1);
open/read/write/close;        /* persistent state in /fs/apps */
sleep_ms(16);
```

That is the intended pattern for small user GUI programs: inspect the
framebuffer, draw each frame, submit pixels through the desktop/app protocol or
graphics syscall wrappers, and poll input.
