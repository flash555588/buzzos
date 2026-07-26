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
top-right indicator between `EN` and `ZH`. In Chinese mode, type full pinyin,
then press Space/Enter for the first candidate or `1`-`9` to choose another.
Backspace edits the composition and Escape cancels it. The desktop owns the
composition and candidate panel, then sends one `GUIAPP_EVT_TEXT` UTF-8 commit
to the focused application. TextEdit, Browser, Files dialogs, Calculator, and
the desktop Terminal all use this common protocol rather than private IMEs.

## Window Behavior

The desktop supports click-to-focus and raise, title-bar dragging, edge and
corner resizing, minimize, maximize, close, mouse wheel scrolling, draggable
scrollbars, and app resize events.

Double-click a title bar to maximize or restore its window. Dragging a
maximized title bar down restores the saved window size under the pointer so it
can be repositioned in one gesture. Dragging a title bar to the left or right
screen edge tiles the window into that half of the work area; dragging to the
top edge maximizes it. A restrained outline previews the destination while the
pointer remains in direct control of the window.

Alt+Tab and Shift+Alt+Tab cycle visible windows in both directions. Alt+F4
closes only the focused window. Super+Left/Right tiles it, Super+Up maximizes
it, and Super+Down restores an arranged window or minimizes a freeform one. A
tap of Super toggles Applications; unsupported Super chords are reserved by
the desktop instead of leaking a character into the focused app.
Plain Tab is delivered to the focused application for fields, indentation, and
other app-local navigation. Escape cancels IME composition, clears launcher
search, dismisses desktop UI, or reaches the focused app; Ctrl+Alt+Esc is the
explicit console-development shortcut that ends the desktop session.

## System Shell

The top bar is a persistent wayfinding and status surface. `BuzzOS` is a
clickable Applications control, the center label names the focused window, and
the right status cluster shows the current input mode and UTC time.

Click the status cluster to open an anchored control center. It shows the full
UTC date, active display mode/backend, and running GUI-app count. Its actions
toggle English/Chinese input, open the built-in display Settings window, and
launch System Monitor. Arrow keys or Tab move between actions, Enter activates
one, and Escape or a click outside closes the panel. Keyboard-triggered shell
actions are immediate and do not add decorative motion.

Right-click an application content area to open the desktop-owned `Copy`,
`Paste`, and `Cut` menu. The clipboard stores UTF-8 text and is shared across
applications. TextEdit supports mouse drag selection and highlights the exact
UTF-8 selection; Browser, Files dialogs, and Calculator copy/cut their current
field. Paste is delivered through the same `GUIAPP_EVT_TEXT` path used by the
system input method.

## Pixel Format (Modern Truecolor Path)

BuzzOS follows the modern desktop model: **working buffers and scanout are
32-bit truecolor (`0x00RRGGBB`)**, not an 8-bit indexed UI palette.

| Layer | Format |
| --- | --- |
| App pixels / `guiapp` SHM | `uint32_t` RGB32, 4 bytes/pixel |
| Desktop backbuffer (`/bin/gui`) | RGB32 |
| Kernel `fb_blit` / `fb_fill` / `fb_text` | RGB32 arguments and blits |
| Bochs VBE / Limine linear FB | 32 bpp modes |
| virtio-gpu 2D | RGBX resource + dirty `TRANSFER`/`FLUSH` |
| Desktop compose | Prefer **zero-copy**: `gfx_map_surface` maps scanout into the compositor; `gfx_present` only flushes damage (GPU) or is a no-op (linear FB) |
| App pixel buffers | **Dynamic** via `appui_pixels_ensure` — sized to the current window (+slack), not a static `GUIAPP_MAX_W×MAX_H` reservation |

There is **no** 8-bit indexed framebuffer path (boot FB must be 16/24/32 bpp;
GUI and scanout are RGB32). Theme colors in `palette.h` are literal RGB;
`plt_blend` blends in RGB. The text console keeps a small **VGA-16 RGB table**
locally for character attributes only.

### GPU usage model (virtio-gpu 2D)

BuzzOS does not have a 3D or hardware window compositor. “Full use” of the
available GPU means:

1. Guest scanout memory is the composition target (`USER_DISPLAY_START` map).
2. Software draws RGB32 directly into that memory (no intermediate full-screen
   blit into the kernel on the hot path).
3. Each damaged region is uploaded once via `TRANSFER_TO_HOST_2D` +
   `RESOURCE_FLUSH` (`gfx_present`).
4. If mapping fails, the desktop falls back to a private backbuffer +
   `fb_blit_stride` (extra copy).

SHM slots are sized for a full-screen RGB32 surface
(`USER_SHM_SLOT_SIZE` ≈ 10 MiB: header + 1920×1200×4).

## Live Resize And Composition (Design Compromises)

This section records intentional trade-offs in `/bin/gui` and `guiapp`, not
temporary hacks. Revisit them only with a clear upgrade path (for example GPU
filtering), not by re-enabling known-bad shortcuts.

### Goals (aligned with modern compositors)

- Window chrome geometry updates immediately while the user drags an edge.
- Apps receive live size changes (`GUIAPP_EVT_RESIZE`) so they can re-layout.
- Until the app presents a matching frame, the desktop must not show torn or
  wrongly strided pixels.
- Maximize, display-mode changes, and mouse-up always push a final size.

### What we do today

| Layer | Behavior |
| --- | --- |
| Configure publish | Desktop writes the latest content size into the shared surface header (`configure_width` / `configure_height`) on geometry changes. |
| Event path | `guiapp_read_event` overlays those fields onto `INIT` / `RESIZE` so queued intermediate sizes still deliver **current** geometry (coalesce). |
| In-flight limit | At most one RESIZE is outstanding per app until a frame is presented (`resize_inflight`). Paces configures to the app’s present rate instead of flooding the event pipe every mouse sample. |
| Force sync | Mouse-up after edge drag, maximize, and mode change call `sync_app_size(..., force)` so the final size is never stuck behind in-flight. |
| 1:1 UI blit | Normal apps (`GUIAPP_FRAME_FULL` / `DIRTY`) are composited **1:1** into the content rect (top-left). If the surface is temporarily smaller, fill body-colored margins; if larger, clip. **No fractional nearest-neighbor stretch** of text/UI while the surface lags the chrome. |
| Seqlock blit | Pixel copy always takes `shared->width` / `shared->height` **inside** the sequence lock (`blit_shared_1to1` / `blit_shared_scaled`). Never use a stale session `surface_w` as row stride. |

### Why not “stretch the last buffer like DWM / Wayland”?

Modern desktops **do** scale the previous client buffer to the new window size
while the client catches up. That looks smooth when scaling is GPU-filtered
(bilinear or better).

BuzzOS software-composes RGB32 with nearest-neighbor scaling only (no cheap
bilinear filter). Fractional stretch (especially ratios near 1, such as
501/500) still turns high-frequency UI and text into **moiré / banding** that
shimmers every mouse pixel during a drag. That is a product choice under
current constraints:

- **Keep:** 1:1 blit + solid margins while the app lags (may flash a body-color
  strip; no moiré).
- **Do not re-enable:** fractional nearest-neighbor live-resize stretch for
  normal UI, unless a higher-quality filter exists.
- **Later upgrade path:** filtered stretch preview (true modern live-resize
  look) *in addition to* the seqlock stride rules below—not instead of them.

### Critical fix: SHM stride vs session cache (FileManager striping)

Heavy apps (Files, Browser, Paint, …) take longer to re-layout and present.
During live resize they often write a **new** buffer size into SHM before the
desktop session fields `surface_w` / `surface_h` are updated from the frame
pipe. If composition memcpy’s with the **old** width as stride against a
**new** row layout, every scanline misaligns → diagonal **stripes / 花纹**.

TextEdit is small and fast, so the race window is short; denser apps hit it
reliably.

**Rule (do not regress):** under a stable even `shared->sequence`, read
`shared->width` and `shared->height` in the same critical section as the pixel
copy; retry if sequence or dimensions change. Session dimensions remain for
damage bookkeeping and policy, not as the sole source of truth for SHM layout
at blit time.

### How this differs from “modern OS” defaults

| Topic | Typical modern OS | BuzzOS compromise |
| --- | --- | --- |
| Live preview while client lags | GPU-scale last buffer to new size | 1:1 + body margins (no cheap filtered scale) |
| Continuous configure during drag | Yes | Yes, but one in-flight RESIZE; size coalesced via SHM configure fields |
| Tear-free present | Compositor + client protocol | Seqlock on shared surface + stride from live header |

### Implementation map

- Desktop: `src/user/bin/gui.c` — `publish_app_configure`, `sync_app_size`,
  `flush_pending_app_resizes`, `blit_shared_1to1`, `blit_shared_scaled`,
  `draw_app_window`, `scaled_view_rect`.
- Protocol: `src/user/libc/guiapp.h` / `guiapp.c` — `configure_width` /
  `configure_height` on `struct guiapp_shared_surface`; overlay in
  `guiapp_read_event`.

Apps that only call `guiapp_read_event` and present full (or dirty) frames get
coalesced live sizes automatically; they should re-layout from event
`width` / `height` and not assume intermediate sizes still in the pipe are
authoritative without the configure overlay.

The desktop Terminal keeps a UTF-8 input mirror for clipboard operations, and
the shell line editor accepts multibyte input instead of filtering bytes above
ASCII. Left/Right, Backspace, and Delete move across complete UTF-8 characters.
Drag with the left mouse button to select UTF-8 terminal output across lines;
the selected glyphs are highlighted. Terminal Copy returns that selection (or
the current edit line when no selection exists), Paste inserts the full
clipboard, and Cut clears the shell edit line through Ctrl+U when no immutable
output selection is active.

The compact taskbar keeps Applications and System pinned, then represents each
running app with a stable monogram tile and running indicator. Hovering a tile
briefly displays the full title declared by the app frame; after one tooltip is
open, moving between adjacent tiles updates it immediately. Clicking the active
tile minimizes its window, while clicking a minimized tile restores and raises
it. If the taskbar is full, the `+` tile opens a complete titled task list.
The desktop supports 10 concurrent external app windows; taskbar overflow is
independent of that capacity.

The desktop starts with Applications as its single visual focal point. System
remains pinned and ready in the taskbar instead of opening a second competing
window at startup.

Applications can also be toggled from the `BuzzOS` top-bar control or with a
tap of Super. The top-right clock/status control opens the system control
center, keeping common system actions available without competing permanent
windows.

Applications has a persistent search field above its scrollable results. With
the launcher focused, typing filters app names, summaries, and executable
names; Up/Down moves within the filtered set and Enter launches the selection.
Backspace edits the query, while Escape clears a non-empty query before it can
exit the desktop. The header and search field stay visible while results
scroll, and an explicit empty state explains how to clear a search with no
matches.

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
