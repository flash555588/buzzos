# NetSurf reference patches (BuzzOS)

Pinned upstream tree: `third_party/netsurf-reference` at
`a471a0d44274ec57fee5e5f30ae59fbd2ad02656` (see `tools/fetch-netsurf.ps1`).

These patches are applied on top of that revision every time the fetch script
runs. They stay out of the upstream git history so the checkout can remain a
plain detached pin.

| Patch | Purpose |
|-------|---------|
| `0001-buzzos-netsurf-reference.patch` | Optional author stylesheets must not fail the whole HTML document when fetch is unavailable (e.g. HTTPS without TLS); clearer error logging; scheduler treats zero-delay callbacks as due immediately so 100 Hz BuzzOS clocks do not serialize layout. |

Do not hand-edit the pinned tree without also updating this patch series.
