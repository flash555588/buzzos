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
/fs/apps/filemanager
/fs/apps/filemanager.app
/fs/apps/filemanager.readme
/fs/apps/browser
/fs/apps/browser.app
/fs/apps/browser.readme
```

Current default apps:

| App | Purpose |
| --- | --- |
| TextEdit | Plain text editor. The editing area resizes with the window, supports Enter, cursor movement, horizontal and vertical scrollbars, and saves to `/fs/textedit.txt`. |
| Paint | Bitmap drawing tool. The canvas and toolbar resize with the window, with brush, eraser, line, rectangle, fill, and continuous strokes. |
| Calculator | Expression calculator with decimals, parentheses, and normal arithmetic precedence. |
| Files | File manager with location shortcuts, navigation history, file operations, and desktop-mediated opening in TextEdit. |
| Browser | Small HTTP browser with URL history, redirects, HTML-to-text rendering, scrolling, and UTF-8 page text. |

## UTF-8 Text

GUI text is decoded as UTF-8. ASCII uses the built-in fast-path font; common
Chinese characters, Latin extensions, Greek, Cyrillic, Japanese kana, and
punctuation are served from one packed kernel font. CJK glyphs use double-cell
width. TextEdit preserves UTF-8 bytes and moves or deletes by complete code
point; Browser wraps decoded page text by pixel width. Open `/fs/utf8.txt` in
Files for a built-in multilingual display sample.

The desktop includes a system input method. Press `Ctrl+Space` to switch the
top-right indicator between `英` and `中`. In Chinese mode, type full pinyin,
then press Space/Enter for the first candidate or `1`-`9` to choose another.
Backspace edits the composition and Escape cancels it. The desktop owns the
composition and candidate panel, then sends one `GUIAPP_EVT_TEXT` UTF-8 commit
to the focused application. TextEdit, Browser, Files dialogs, Calculator, and
the desktop Terminal all use this common protocol rather than private IMEs.

## Window Behavior

The desktop supports click-to-focus and raise, title-bar dragging, edge and
corner resizing, minimize, maximize, close, mouse wheel scrolling, draggable
scrollbars, and app resize events.

Right-click an application content area to open the desktop-owned `Copy`,
`Paste`, and `Cut` menu. The clipboard stores UTF-8 text and is shared across
applications. TextEdit supports mouse drag selection and highlights the exact
UTF-8 selection; Browser, Files dialogs, and Calculator copy/cut their current
field. Paste is delivered through the same `GUIAPP_EVT_TEXT` path used by the
system input method.

The desktop Terminal keeps a UTF-8 input mirror for clipboard operations, and
the shell line editor accepts multibyte input instead of filtering bytes above
ASCII. Left/Right, Backspace, and Delete move across complete UTF-8 characters.
Drag with the left mouse button to select UTF-8 terminal output across lines;
the selected glyphs are highlighted. Terminal Copy returns that selection (or
the current edit line when no selection exists), Paste inserts the full
clipboard, and Cut clears the shell edit line through Ctrl+U when no immutable
output selection is active.

The task Dock displays titles declared by each app frame. It shows several
tasks inline, exposes all open app windows through More, and displays the full
title while hovering a task. The desktop supports 10 concurrent external app
windows; the expandable Dock is independent of that capacity.

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
apps info filemanager
apps info browser
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

## Files And Cross-App Open

Files starts in /fs. Use Back and Up for navigation, the Places sidebar for
common roots, Enter or a second click to open an item, and the toolbar for
create, rename, and confirmed delete operations. Executables in /fs/apps
launch as GUI apps. Other regular files are opened in TextEdit.

Cross-app opening uses GUIAPP_FRAME_LAUNCH. The desktop validates the target,
creates a managed app window, and passes the document path after the GUI
transport arguments. This keeps process creation and window ownership in the
desktop instead of allowing apps to bypass the window manager.

Files identifies executables by the ELF magic rather than filename shape. An
ELF with a sibling GUI manifest is launched through `GUIAPP_FRAME_LAUNCH`;
other `/fs` ELF programs use `GUIAPP_FRAME_EXEC` and run in the desktop
Terminal. This prevents the desktop from blocking while waiting for a normal
CLI program to send a GUI frame. The desktop application list likewise ignores
bare executables without a manifest.

Each managed app has a dedicated frame-reader thread. The desktop event loop
only queues mouse, keyboard, resize, IME, and clipboard events; it never waits
synchronously for the app's next frame. A slow Browser DNS/TCP/HTTP request
therefore keeps its previous pixels on screen while the pointer, Dock, input
method, and other windows remain responsive. Protocol termination is detected
by the reader and reaped by the desktop loop.

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
