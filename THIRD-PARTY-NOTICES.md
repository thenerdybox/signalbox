# Third-Party Notices

## obs-plugintemplate (build scaffold)

This project's CMake build scaffold is derived from the official OBS Studio
plugin template:

- Project: `obsproject/obs-plugintemplate`
- Source: https://github.com/obsproject/obs-plugintemplate
- License: GPL-2.0-or-later

Files retained from the template with only mechanical edits (project name,
Windows SDK version, trimming of macOS/Linux slices which are out of scope
for this Windows-first v1.0):

- `cmake/common/*.cmake`
- `cmake/windows/*.cmake`
- `cmake/windows/resources/resource.rc.in`
- The overall `buildspec.json` schema and `CMakeLists.txt` / `CMakePresets.json`
  structure (option names, target wiring, preset shape)

These files are generic OBS build plumbing - dependency fetching, compiler
flags, install layout - not SignalBox's own behavior. Using them is
the template's documented intended use (`obsproject/obs-plugintemplate` is
published specifically to be copied into new plugin projects).

All of SignalBox's own source (`src/`, `data/`) is original and
licensed MIT (see `LICENSE`), except where noted otherwise.

## Open licensing question

Because this plugin links against `libobs` (GPL-2.0-or-later) via
`obs-frontend-api`/`libobs` at build time, the compiled binary may carry GPL
obligations independent of the license on this source tree. This is exactly
why the upstream template ships GPL-2.0-or-later by default. TheNerdyBox has
not made a final call on this for SignalBox - see the licensing note
at the bottom of `LICENSE`. Flagging it here rather than resolving it
silently.

## Planned future dependency: nlohmann/json

`DESIGN.md` specifies a vendored single-header JSON library
(`nlohmann/json`, MIT license, https://github.com/nlohmann/json) for parsing
Epic's `.item` manifests, config, and Twitch API payloads. Not yet vendored -
the current stubs don't need it. Add it here when a provider or the Twitch
client needs real JSON parsing.
