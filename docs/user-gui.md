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

Dragging a window into a screen edge snaps it: the top maximises, the left and
right halve the work area. A translucent accent preview shows the target while
the drag is in flight. The zone is keyed off the pointer rather than the window
bounds, so an already-wide window does not snap merely by drifting left. The
pre-snap bounds are kept in `restore`, so the maximise button and a later drag
put the window back where it was.

Right-click an application content area to open the desktop-owned `Copy`,
`Paste`, and `Cut` menu. The clipboard stores UTF-8 text and is shared across
applications. TextEdit supports mouse drag selection and highlights the exact
UTF-8 selection; Browser, Files dialogs, and Calculator copy/cut their current
field. Paste is delivered through the same `GUIAPP_EVT_TEXT` path used by the
system input method.

## Visual Design (Modern Desktop)

The shell and applications share one dark theme in the contemporary desktop
idiom — rounded corners, layered translucent surfaces, monoline icons, subtle
hover fills. It borrows that visual language rather than reproducing any one
system's UI, and none of it reaches below user space: BuzzOS is POSIX-leaning
underneath, and this is a theme, not a compatibility target.

Three layers, in dependency order:

| Layer | File | Owns |
| --- | --- | --- |
| Tokens | `src/user/libc/palette.h` | `UI_*` colors, radii, elevation, motion, taskbar metrics |
| Rendering kernel | `src/user/libc/uikit.h` (+`_text.h`, `_icon.h`) | AA rounded rects, gradients, soft shadow, acrylic blur, scaled glyph cache, 31 line icons |
| Widgets | `src/user/libc/appui.h` | Buttons, fields, list rows, tabs, checkbox, progress, cards, the shared scrollbar |

Legacy `THEME_*` macros still exist and are aliased onto `UI_*` tokens, so
applications track the theme before they are individually migrated.

### Layout

There is no top bar. The work area starts at y=0 and ends at a full-width 48px
taskbar: Start button and window buttons centred, tray (IME badge, clock) right.
`work_area()` is the single source of truth for what a maximised or snapped
window may occupy.

Taskbar geometry is produced once by `taskbar_items()` and consumed by both the
painter and the hit tester. The same discipline applies to the Start menu tile
grid and to `appui_scroll_thumb`. This is deliberate: the previous dock derived
its button rects twice from the same inputs, so a layout tweak had to be
mirrored in two places or clicks landed on the wrong button.

### Anti-aliasing and cost

Rounded corners are 4x4 supersampled *inside the corner blocks only* — a
rounded rect costs one plain fill for its body plus `4*rad²` coverage tests.
A signed distance field over the whole rect would pay a square root per pixel
to compute a value that is 0 or 255 everywhere except a two-pixel band.

Acrylic blurs at 1/4 resolution (downsample → 3 box passes → bilinear upsample
→ tint → deterministic grain). A box kernel that small is invisible once
upscaled, and it makes a full-width taskbar backdrop cost a few thousand pixels
instead of a hundred thousand. If scratch memory is unavailable it degrades to
a solid tinted panel rather than failing to draw.

`ui_shadow` takes the **caster** rect, not the offset shadow rect, and
attenuates by the caster's own rounded coverage. Attenuating by the offset rect
instead leaves the `dy`-pixel strip directly under the window covered by
neither the shadow nor the window, so the wallpaper shows through as a bright
seam. That regression is pinned by `tests/uikit_test.c`.

### Text is not auto-scaled

`uikit_text.h` resamples the single 15x28 face into five UI sizes, cropped to
the font's true ink bounds so a glyph's y is the top of the glyph — this is
what retires the `PLT_FONT_Y_SHIFT` fudge for new code.

But `appui_text` still draws at **native** size. Terminal, TextEdit, LuaIDE and
Browser use `KFONT_HEIGHT`/`KFONT_WIDTH` as a character grid; silently
rescaling that call would have shifted every caret, cursor and hit box in those
apps. Scaled sizes are reached through `appui_label`, which applications adopt
per-widget for chrome that is not character-cell layout.

### Testing

`make uikit-test` builds and runs `tests/uikit_test.c` on the host. Both layers
are pure integer pixel math over a caller-supplied buffer, so corner coverage,
clipping, stride handling, glyph metrics, shadow seams and the shared scrollbar
geometry can be checked without booting. It is part of `make verify`.

### GPU presentation

When the device offers virgl, `/bin/gui` composes **directly into a GPU
texture** and presents it as a textured quad instead of copying the frame to
the scanout on the CPU. `gpucomp.h` owns the pipeline; `gpu_present_init()`
retargets `fb` at the texture's mapped backing, so there is no intermediate
copy — the scene is built straight into the memory the upload reads from.

Three properties this relies on:

- **It is optional at every step.** `gpu_present_region()` returns non-zero on
  any failure and `render_region()` falls through to `gfx_present` and then to
  `fb_blit_stride`. A device without virgl, or a mid-session GPU failure,
  degrades rather than blanking the screen.
- **Only the damaged rect is uploaded**, so a blinking caret costs a few
  hundred bytes rather than a full screen. The texture retains its contents
  between frames, exactly as the scanout does.
- **Textures are `B8G8R8X8_UNORM`.** The rest of the GUI stores opaque pixels
  as `0x00RRGGBB` — alpha zero — which under `B8G8R8A8` with SRC_ALPHA blending
  is fully transparent. `X8` forces alpha to 1.0 and ignores the stored byte.
  Per-window opacity comes from a shader constant instead. This is the single
  easiest way to get a black screen on this path.

A mode change tears the GPU path down and brings it back up, because both the
frame texture and the viewport are sized to the old resolution.

**Still on the CPU: the acrylic blur.** It is the most expensive thing the
software compositor does — measured on a fast host, one 480×460 Start-menu
acrylic costs ~1.5 ms and the full-width taskbar ~0.4 ms, against 0.07 ms for
a full-screen fill; on a `-mno-sse` guest the gap is far wider. Moving it needs
render-to-texture (blur horizontally into an offscreen target, then sample
*that* vertically), which means binding a framebuffer backed by a scratch
texture between passes. Drawing both passes straight onto the scanout would
blur the first pass' own output in place and produce a smear, not a Gaussian.
`GPUCOMP_FS_BLUR` is the shader for it; the encoder does not yet emit the
framebuffer switch.

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

### GPU usage model (virtio-gpu)

BuzzOS uses virtio-gpu in two modes, chosen at boot from the device's feature
bits.

**2D path (always available).** Guest scanout memory is the composition
target (`USER_DISPLAY_START` map); software draws RGB32 directly into it and
each damaged region is uploaded once via `TRANSFER_TO_HOST_2D` +
`RESOURCE_FLUSH` (`gfx_present`). If mapping fails, the desktop falls back to
a private backbuffer + `fb_blit_stride` (extra copy).

**3D path (virgl, when the host offers it).** `VIRTIO_GPU_F_VIRGL` gives the
guest a real host GL context. BuzzOS drives it by hand-encoding virgl command
streams — there is **no Mesa and no guest OpenGL stack**; virgl is a command
protocol, and shaders are TGSI *text* that the host's virglrenderer translates
to GLSL. This makes hardware alpha blending, bilinear filtering, and
render-to-texture passes available to the compositor.

Division of labour:

| Layer | Responsibility |
| --- | --- |
| `src/kernel/drv/virtio_gpu.c` | Transport: virtqueue, feature negotiation, 2D scanout |
| `src/kernel/drv/virtio_gpu_3d.c` | Context, GPU resources + guest backing, `SUBMIT_3D` passthrough |
| `src/kernel/syscall/sys_gpu3d.c` | Display-owner-gated syscalls |
| `src/user/libc/virgl.h` | Command-stream encoder, TGSI shaders, float vertex data |

Encoding lives in user space deliberately: TGSI is text and vertex data is
floating point, neither of which belongs in a `-mno-sse` freestanding kernel.

Resource backings are mapped into the display owner at `USER_GPU_START`, so a
texture upload is *zero-copy*: write pixels through the mapping, then
`gpu3d_upload()` only the damaged box. Total backing is capped by
`USER_GPU_BUDGET_BYTES` (64 MiB) so a runaway compositor cannot starve the
256 MiB managed pool.

Not available on this host: `VIRTIO_GPU_F_RESOURCE_BLOB` is not offered under
Windows (no udmabuf), so blob/shared-memory resources are out of reach. The
cursor queue exists (2 queues) and a hardware cursor plane is still open.

### virgl pitfalls (learned the hard way)

**Failures are silent.** A malformed command stream still returns
`VIRTIO_GPU_RESP_OK_NODATA` from `SUBMIT_3D`; the frame just comes out empty
or wrong. Budget debugging time accordingly, and verify every protocol
constant against real headers rather than from memory. Constants that bit us:
`RESOURCE_CREATE_3D` is `0x0204` and `SUBMIT_3D` is `0x0207` (easy to
transpose); `PIPE_BLENDFACTOR_SRC_ALPHA` is **3** and `INV_SRC_ALPHA` is
**0x13** — the enum counts from 1 and the inverted half is OR'd with
`INVERT_BIT` (0x10). Using 4/0x14 silently selects `DST_ALPHA` /
`INV_DST_ALPHA`, which against an opaque target collapses to `src*1 + dst*0`
and renders fully opaque with blending nominally on.

**TGSI needs explicit declarations.** An undeclared `IMM[0]` makes the
host-side parse fail — silently. `num_tokens` in the shader packet sizes the
host's token array before parsing; under-counting also fails silently, so
derive it from the text length with slack instead of guessing.

**Alpha convention clash.** The rest of BuzzOS stores opaque pixels as
`0x00RRGGBB` — the alpha byte is *zero*. Sampled as `B8G8R8A8` with
`SRC_ALPHA` blending, every texel is fully transparent and nothing appears.
GPU-bound surfaces must set alpha to `0xFF`, or be uploaded as
`B8G8R8X8_UNORM`, which forces alpha to 1.0 and ignores the stored byte.

**Orientation belongs in exactly one place.** BuzzOS puts it in the vertex
data: position Y grows downward (screen order) and texcoord V is emitted
flipped, so texture row 0 lands at the top. `virgl_set_viewport` therefore
stays in plain GL orientation — flipping there *as well* cancels out and
silently un-flips the result.

Verify the pipeline end to end with `gputest` from the shell (requires
`-device virtio-vga-gl`): it draws a textured checkerboard, a translucent bar
over it, and an opaque reference bar.

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

On the **software path** BuzzOS composes RGB32 with nearest-neighbour scaling
only (no cheap bilinear filter). Fractional stretch (especially ratios near 1,
such as 501/500) turns high-frequency UI and text into **moiré / banding**
that shimmers every mouse pixel during a drag. So under software composition:

- **Keep:** 1:1 blit + solid margins while the app lags (may flash a
  body-color strip; no moiré).
- **Do not re-enable:** fractional nearest-neighbour live-resize stretch for
  normal UI.

On the **virgl path** this constraint is lifted: the host GL context provides
real bilinear filtering (`PIPE_TEX_FILTER_LINEAR`), so scaling the previous
client buffer is exactly the modern live-resize behaviour and does not produce
moiré. The rule above is a property of software composition, not a permanent
product choice — it stays in force whenever virgl is unavailable.

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
