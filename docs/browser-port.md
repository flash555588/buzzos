# BuzzOS browser engine port

BuzzOS is moving from its small hand-written HTTP/HTML viewer to a NetSurf-based
browser engine. The existing '/fs/apps/browser' remains the bootstrapping shell
and regression target until the native engine reaches feature parity.

## Upstream baseline

- Project: NetSurf
- Revision: a471a0d44274ec57fee5e5f30ae59fbd2ad02656
- Fetch: powershell -File tools/fetch-netsurf.ps1
- BuzzOS patches: third_party/netsurf-patches/
- License: GPL-2.0 with the upstream OpenSSL exception

The reference checkout is deliberately ignored by Git (see `.gitignore`).
`tools/fetch-netsurf.ps1` pins the upstream revision, then applies the tracked
patch series under `third_party/netsurf-patches/`. Ported frontend code lives
under `src/user/ports/netsurf/`, with upstream copyright and license retained.

NetSurf was selected because its framebuffer frontend has no mandatory desktop
GUI toolkit, and its engine is already split into HTML5 parsing (Hubbub), DOM
(LibDOM), CSS parsing and selection (LibCSS), image decoders, fetching, layout,
and frontend plotter interfaces. Dillo and Links2 are smaller, but neither has a
usable JavaScript path.

## Port boundary

The first native target is 'netsurf-buzzos', using:

- a RAM libnsfb surface copied into a GUI application frame;
- BuzzOS GUI events translated to NetSurf mouse and key events;
- monotonic_ms() for the scheduler;
- VFS-backed resource, cookie, history, and cache files;
- a fetch backend built on BuzzOS sockets initially, then libcurl when the
  required socket/POSIX surface is available;
- TLS through a maintained TLS library rather than ad-hoc certificate handling.

## Required milestones

1. Platform layer: time, memory, strings, files, events, and RAM framebuffer.
2. Core document path: parserutils, wapcaplet, Hubbub, LibDOM, LibCSS, nsutils.
3. Rendering: HTML/CSS layout, fonts, PNG/JPEG/GIF/WebP/SVG.
4. Web platform: redirects, cookies, cache, forms, downloads, history.
5. Security: HTTPS, certificate validation, hostname checks, entropy, clock.
6. Script path: Duktape and NetSurf DOM bindings, with per-page memory/time
   limits. This is useful but does not claim full Chromium-compatible JS.
7. Compatibility service for sites that require browser APIs outside NetSurf's
   scope. This is the practical route to broad daily-web compatibility while
   keeping the native engine small.

## Current kernel gaps

The browser needs stronger primitives than the original viewer: a wall clock,
cryptographic random source, non-blocking sockets or polling, richer file
metadata, and larger executable/file limits. User processes now have a dedicated
256 MiB virtual window at 0x20000000 through 0x30000000, replacing the original
16 MiB window. SYS_MONOTONIC_MS is the first timing primitive and is used for
event-loop scheduling; it is not a substitute for the trusted wall clock needed
for TLS certificate validation.

## Working core build

Run 'powershell -File tools/fetch-netsurf.ps1' once, then build normally.
'tools/build-netsurf-core.ps1' cross-compiles the pinned parserutils,
wapcaplet, Hubbub, LibDOM, LibCSS, and nsutils sources. '/bin/nshtmltest'
executes those libraries inside BuzzOS and verifies an HTML document containing
CSS, a form, an input, and an image node.
