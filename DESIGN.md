# SignalBox — Design Specification

**Product:** Free OBS Studio plugin (Windows-first) that detects the game or application the streamer is running and automatically sets the Twitch stream category.
**Owner:** TheNerdyBox / SasiRawr. Not monetised; stays free. Differentiator: it is actually maintained.
**Status:** Implementation-ready design. No code in this document is production code; snippets are illustrative pseudo-code only.

**Clean-room statement:** This design was produced exclusively from public, documented sources: the OBS Studio plugin API and official `obs-plugintemplate`, Microsoft Win32 documentation (Microsoft Learn / MSDN), Valve's published Steam file formats, Epic Games Launcher's on-disk manifest format, GOG/Ubisoft registry conventions as publicly documented, and the Twitch Helix API and OAuth documentation. No third-party plugin source code was consulted. Sources are cited inline.

---

## 1. Detection

### 1.1 Design principle

Never guess which `.exe` in an install folder "looks like the game." Instead:

1. Build an **install index** from each store platform's own metadata (Steam ACF/VDF, Epic `.item` manifests, GOG/Ubisoft registry, generic Uninstall registry). The platform already knows the game's name, its install directory, and (for Epic) its exact launch executable.
2. Enumerate **running processes** and resolve each one's full image path.
3. **Match running process paths against indexed install roots** (longest-prefix match), filter out documented helper executables, and rank the survivors.
4. Map the winning game's **platform-supplied display name** to a Twitch category via a three-tier resolver (Section 1.5).

The game's identity comes from the platform's metadata, not from filename heuristics. Filename heuristics appear only as a last-resort tie-breaker *within* an already-identified game directory.

### 1.2 Install index — platform providers

Each provider implements one interface:

```
struct InstalledGame {
    std::wstring displayName;    // platform's own name for the game
    std::wstring installRoot;    // absolute, normalized directory
    std::wstring launchExe;      // absolute path if known, else empty
    Platform     platform;       // Steam, Epic, GOG, Ubisoft, Generic
    std::wstring platformId;     // appid / catalog id / product id
};
class IInstallProvider { virtual std::vector<InstalledGame> enumerate() = 0; };
```

The index is rebuilt at plugin load, every 10 minutes, and on demand (dock "Rescan" button). Rebuild cost is a handful of small file reads and registry queries — negligible.

#### Steam provider

- **Find Steam root:** registry `HKCU\Software\Valve\Steam` value `SteamPath` (fallback `HKLM\SOFTWARE\WOW6432Node\Valve\Steam` value `InstallPath`).
- **Find all libraries:** parse `<SteamPath>\steamapps\libraryfolders.vdf`. This is Valve's text KeyValues (VDF) format — nested `"key" { ... }` / `"key" "value"` pairs. Each numbered child block has a `"path"` value naming a library root. Format documented on the Valve Developer Community wiki ("KeyValues", "libraryfolders.vdf").
- **Enumerate installed apps:** for each library root, read `<library>\steamapps\appmanifest_<appid>.acf`. ACF is the same KeyValues text format. Relevant keys inside the `"AppState"` block:
  - `"appid"` — Steam app id.
  - `"name"` — the game's display name (this is our category-mapping input).
  - `"installdir"` — folder name under `<library>\steamapps\common\`.
  - `"StateFlags"` — bitfield; treat `4` (fully installed) as installed, skip apps mid-download.
  - `installRoot = <library>\steamapps\common\<installdir>\`. `launchExe` is unknown from the ACF (it does not record the executable); Steam matching is by directory prefix.
- **Corroborating signal (optional, never primary):** `HKCU\Software\Valve\Steam` value `RunningAppID` (DWORD; 0 when no game is running). This is a widely known but *unofficial* value; the design treats it purely as a confidence booster for a Steam candidate already found by path matching, and functions fully without it.
- **VDF/ACF parser:** write a ~100-line tolerant recursive-descent parser for quoted-token KeyValues (tokens: `"..."` with `\\` and `\"` escapes, `{`, `}`; `//` comments). Do not take a dependency for this.

#### Epic Games provider

- Manifests live at `%PROGRAMDATA%\Epic\EpicGamesLauncher\Data\Manifests\*.item` — one JSON file per installed app. Relevant fields:
  - `DisplayName` — game name (category-mapping input).
  - `InstallLocation` — absolute install root.
  - `LaunchExecutable` — **relative path of the real game binary**. This is the gold case: `launchExe = InstallLocation + "\\" + LaunchExecutable`, so Epic matches are exact-path, not prefix.
  - `AppName` / `MainGameAppName` — catalog ids; skip entries where `AppName != MainGameAppName` (DLC).
- Secondary source (cross-check only): `%PROGRAMDATA%\Epic\UnrealEngineLauncher\LauncherInstalled.dat`, a JSON array of `{AppName, InstallLocation}`.
- Parse with a small vendored JSON library (nlohmann/json single header) — also needed for config and Helix.

#### GOG provider

- Registry: `HKLM\SOFTWARE\WOW6432Node\GOG.com\Games\<productId>` (64-bit view: without WOW6432Node — enumerate both with `KEY_WOW64_32KEY`/`KEY_WOW64_64KEY`). Values per game key:
  - `gameName` — display name.
  - `path` — install root.
  - `exe` — full path of the launch executable (exact-match case, like Epic).

#### Ubisoft Connect provider

- Registry: `HKLM\SOFTWARE\WOW6432Node\Ubisoft\Launcher\Installs\<installId>` value `InstallDir` — install root only; no name, no exe.
- `displayName` fallback: last path component of `InstallDir` (e.g. `...\games\Anno 1800\` → "Anno 1800"). Ubisoft install folders are human-named, so this feeds the resolver acceptably; the user-override tier (1.5) covers misses.

#### Generic Uninstall provider (covers EA app, Battle.net, standalone installers)

- Enumerate `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*` and the WOW6432Node twin, plus HKCU equivalents. Documented convention: values `DisplayName` and `InstallLocation` (Microsoft Learn: "Uninstall Registry Key").
- Include entries only if `InstallLocation` is non-empty and exists. This provider is *low priority* (Section 1.4): it catches EA titles and one-off installs without special-casing EA Desktop's undocumented encrypted install store.
- Filter obvious non-games by `Publisher`/`DisplayName` denylist is **not** attempted — matching is driven by running processes, so a Visual C++ Redistributable entry in the index is harmless (nothing ever runs from its `InstallLocation`).

#### Xbox / Game Pass (deferred to v1.1)

- MSIX-packaged processes can be identified via `GetPackageFamilyName(hProcess, ...)` (Microsoft Learn), which works with a `PROCESS_QUERY_LIMITED_INFORMATION` handle. Package family name → display name requires `PackageManager`/`AppxManifest.xml` inspection. Specified as a future provider; not v1.0 scope.

### 1.3 Process enumeration layer

- **Enumerate PIDs:** `CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS)` + `Process32FirstW/NextW` (Microsoft Learn: "Taking a Snapshot and Viewing Processes"). Gives pid, parent pid, and exe basename cheaply — the basename lets us skip obviously irrelevant system processes before opening handles.
- **Resolve full image path:** `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)` then `QueryFullProcessImageNameW(h, 0, buf, &size)`.
  - **Why this access right:** Microsoft Learn, "Process Security and Access Rights": `PROCESS_QUERY_LIMITED_INFORMATION` is a deliberately minimal right that is granted for protected processes where `PROCESS_QUERY_INFORMATION` and `PROCESS_VM_READ` are denied. Anti-cheat drivers (EAC, BattlEye, Vanguard) strip handle rights to protected game processes via ObRegisterCallbacks; `PROCESS_VM_READ` is denied *regardless of elevation*, but reading the image path via QUERY_LIMITED succeeds. `QueryFullProcessImageNameW` is documented (Microsoft Learn) to require only `PROCESS_QUERY_LIMITED_INFORMATION`.
  - **Never** call `ReadProcessMemory`, `EnumProcessModules`, or inject anything. Nothing in this plugin needs more than the image path. This is both the correct engineering choice and the anti-cheat-safe one.
- **Failure handling:** `OpenProcess` returning NULL (ACCESS_DENIED on some system processes) is normal — skip silently. Normalize paths with `GetFinalPathNameByHandle` is unnecessary; compare case-insensitively with normalized separators and resolve the index roots once via `GetFullPathNameW`.
- **Foreground signal:** `GetForegroundWindow()` → `GetWindowThreadProcessId()` (Microsoft Learn) gives the pid owning the foreground window. Used only as a tie-breaker (1.4), never as the primary detector — alt-tabbing must not change detection.
- Poll cadence: every 5 s on the worker thread. A snapshot plus a few hundred `OpenProcess` calls costs single-digit milliseconds.

### 1.4 Matching and ranking

For each running process path:

1. **Exact `launchExe` match** (Epic, GOG) → candidate, confidence HIGH.
2. Else **longest-prefix match** against index `installRoot`s → candidate, confidence by platform: Steam HIGH (its `common\<installdir>` roots are game-specific), Ubisoft MEDIUM, Generic/Uninstall LOW.
3. **Helper filter** — discard the process (not the game) if:
   - path contains `\_CommonRedist\` (Steamworks redistributable convention, Valve docs) or `\Redist\`, `\directx\`, `\vcredist`;
   - basename matches a shipped denylist of documented helper binaries: `UnityCrashHandler*.exe` (Unity docs), `CrashReportClient.exe` (Unreal docs), `crashpad_handler.exe` (Chromium crashpad), `EasyAntiCheat*.exe`, `BEService*.exe`, `unins*.exe`, `*setup*.exe`, `*installer*.exe`, `dxsetup.exe`, `vconsole*.exe`;
   - basename matches a shipped launcher denylist: `steam.exe`, `steamwebhelper.exe`, `EpicGamesLauncher.exe`, `upc.exe`, `UbisoftConnect.exe`, `GalaxyClient.exe`, `EADesktop.exe`, `Battle.net.exe`, `RiotClientServices.exe`. (Launchers additionally never match game `installRoot`s, so this list is belt-and-braces.)
   - The denylists ship as a data file (`data/helpers.json`) so fixes are a data PR, not a release.
4. **Rank surviving candidates** (multiple games running is real — e.g. an idle game plus the streamed game):
   1. Foreground-window pid belongs to the candidate (stable across ≥2 polls) — strongest.
   2. Confidence tier (HIGH > MEDIUM > LOW).
   3. Steam `RunningAppID` corroboration.
   4. Largest process working set (via `GetProcessMemoryInfo`, which also accepts QUERY_LIMITED handles — Microsoft Learn) — a real game dwarfs a stub.
   5. Most recently started (`GetProcessTimes`, QUERY_LIMITED-compatible).
5. Emit at most one `DetectedGame{InstalledGame, pid, confidence}` per poll, or `None`.

### 1.5 Mapping a detected game to a Twitch category

Input: the platform's `displayName` (e.g. Steam ACF `"name" "ELDEN RING"`). Twitch categories are looked up by name via Helix. Three tiers, first hit wins:

1. **User overrides** (highest): config map keyed by `platform:platformId` (preferred) or absolute exe path → `{game_id, game_name}`. Set from the dock ("this game is always X"). Persisted forever; survives everything. Also supports "ignore this exe entirely."
2. **Bundled alias table:** `data/aliases.json`, a curated map of normalized platform names → Twitch category names/ids, for known mismatches (regional titles, "GOTY Edition" naming, mod launchers). Maintained in the repo; community PRs welcomed — this table is where the "actually maintained" positioning becomes visible.
3. **Helix resolution:**
   - Normalize: strip `™ ® ©`, collapse whitespace. Do **not** strip edition suffixes by default (too risky: "Definitive Edition" is often the *correct* Twitch listing).
   - `GET /helix/games?name=<exact>` (exact-match endpoint; Twitch API reference "Get Games"). Hit → done.
   - Miss → `GET /helix/search/categories?query=<normalized>` ("Search Categories") and score results: case-insensitive exact > exact-after-normalizing-both > token-set overlap ≥ 0.8. Below threshold → **unmapped**: show "couldn't map <name> — pick a category" in the dock and change nothing. A wrong category set silently is worse than no change.
   - Every successful resolution is written back to a local learned-mappings cache (config), so Helix is consulted at most once per game, ever, unless the user clears it.

---

## 2. Stability — the detection state machine (crash handling is a first-class flow)

**Owner requirement:** when a game crashes, detection must fall through to "nothing detected" and set the fallback category (default **Just Chatting**). This is the required default, not an option. The design problem is doing that *without flapping*, because a crash is very often followed by a relaunch within a minute or two, and `Just Chatting → game → Just Chatting → game` is worse for the streamer than a short delay.

### 2.1 Named tuning constants

All timings are **named, configurable constants** (one struct, persisted in config, surfaced under an "Advanced timing" group in the dock). Nothing below is hardcoded — a parallel rate-limit research pass (`rate-limits.md`, pending; this spec does not wait on it) will inform retuning, and these are the knobs it turns:

| Constant | Default | Meaning |
|---|---|---|
| `POLL_INTERVAL_S` | 5 | Process-scan cadence on the worker thread |
| `CONFIRM_POLLS` | 3 | Consecutive winning polls before a switch-in (→ 15 s) |
| `FOREGROUND_TIEBREAK_POLLS` | 2 | Stability required before foreground breaks a tie |
| `CRASH_GRACE_S` | 120 | Relaunch window after an *abnormal* process exit |
| `CLEAN_EXIT_GRACE_S` | 30 | Relaunch window after a *clean* exit (code 0) |
| `MIN_PATCH_SPACING_S` | 10 | Floor between successive category PATCHes |
| `CHANNEL_SYNC_INTERVAL_S` | 120 | External-change check cadence while live (Section 3.5) |
| `FLAP_BREAKER_N` / `FLAP_BREAKER_WINDOW_S` | 4 / 600 | Auto-changes allowed per window before automation pauses |

### 2.2 States and transitions

States: `IDLE` → `PENDING(g)` → `ACTIVE(g)` → `GRACE(g)` → (`ACTIVE(g)` | `ACTIVE(h)` | `FALLBACK`).

- **Switch-in (`IDLE/FALLBACK → PENDING → ACTIVE`, or `ACTIVE(a) → ACTIVE(b)`):** the same candidate must win `CONFIRM_POLLS` consecutive polls (15 s) before any Twitch call. Kills flaps from launchers briefly spawning the game exe, splash/DRM stubs that exec the real binary, and mis-ranked first polls.
- **Alt-tab / minimise:** explicitly **not** an exit. Detection keys on process *existence*, never on focus; `GetForegroundWindow` is only a tie-breaker between two simultaneously running indexed games (and that tie-break itself needs `FOREGROUND_TIEBREAK_POLLS`). Alt-tabbing to a browser mid-game changes nothing. "Process gone" and "process alive but backgrounded" are different observations by construction.
- **Exit detection (`ACTIVE(g) → GRACE(g)`):** while ACTIVE, DetectionEngine *keeps the QUERY_LIMITED handle open* on the game's pid and checks `WaitForSingleObject(h, 0)` each poll — exit is observed within one poll even if a same-named process appears elsewhere. On exit, call `GetExitCodeProcess` (documented to require only `PROCESS_QUERY_LIMITED_INFORMATION` — Microsoft Learn): exit code `0` → clean quit, start `CLEAN_EXIT_GRACE_S`; any nonzero code (crashes surface codes like `0xC0000005`) → treat as crash, start `CRASH_GRACE_S`. The heuristic isn't perfect (some engines exit nonzero routinely) and only sizes the grace window — it never changes *what* happens, only *when*.
- **Crash leaves the launcher running:** already structurally handled — Steam/Epic/etc. launchers are denylisted by basename *and* never match game install roots, so a surviving launcher can neither hold `ACTIVE` alive nor become a candidate itself. GRACE is driven purely by the dead game's handle, not by "is anything from that vendor still running."
- **During GRACE(g):**
  - `g` reappears (relaunch after crash, DRM restart, mid-session patcher) → re-confirm with `CONFIRM_POLLS`, return to `ACTIVE(g)` with **zero Twitch calls** — the category never moved. This is the anti-flap core.
  - A *different* game `h` confirms → normal switch to `ACTIVE(h)`.
  - Grace expires with nothing detected → **apply the fallback category (default Just Chatting) via PATCH**, log it in the dock (with Undo), enter `FALLBACK`. Fallback category is user-configurable; the *behaviour* is on by default per the owner's requirement, with "hold last category instead" available as an opt-out toggle.
- **Why the fallback delay is deliberately much longer than the detection delay (120 s vs 15 s):** the two directions carry different evidence and different costs. Switch-in acts on *positive* evidence (the process verifiably exists — being right slightly late costs almost nothing). Fallback acts on *absence* of evidence, and absence immediately after presence is most often a crash-relaunch in progress; acting fast here means being visibly wrong on the channel page twice in two minutes. Briefly wrong is worse than slightly late, so the asymmetry is intentional: fast in, slow out.
- **Crash-loop breaker:** if automation performs more than `FLAP_BREAKER_N` category changes within `FLAP_BREAKER_WINDOW_S` (e.g. a game crash-looping), pause automation, keep whatever category is currently set, and show a dock notice ("Category changes paused — <game> appears unstable. Resume?"). Bounded worst case: no crash pattern can generate unbounded viewer-visible churn.
- **Manual lock:** dock toggle suspends all automatic changes (special segments). Every automatic change is logged in the dock with one-click **Undo** (restores the previous category via the same PATCH).
- **Only-while-live option:** by default the plugin also updates category while offline (useful pre-stream; Twitch permits it); a toggle restricts changes to the `OBS_FRONTEND_EVENT_STREAMING_STARTED..STOPPED` window.
- **Redundancy guard:** before any PATCH, compare against last-known channel state; skip no-op updates. Extended by the external-change guard in Section 3.5.

---

## 3. Twitch integration

### 3.1 OAuth: Device Code Grant (DCG) — chosen

Options considered:

| Flow | Needs client secret | Needs redirect endpoint | Refresh token | Verdict |
|---|---|---|---|---|
| Authorization code | **Yes** — cannot ship a secret in a public free plugin | Yes | Yes | Rejected (secret) or requires a hosted token-exchange backend — explicitly a non-goal for this local-first owner |
| Implicit grant | No | Yes — local HTTP listener on `http://localhost` + browser redirect | **No** — token simply expires; user must re-auth | Rejected: no refresh means guaranteed mid-stream re-auth pain; also requires opening a local port and registering redirect URIs |
| **Device Code Grant** | **No** (public client type) | **No** | **Yes** | **Chosen** |

DCG mechanics (Twitch docs: "Getting OAuth Access Tokens — Device Code Grant Flow"):

1. `POST https://id.twitch.tv/oauth2/device` with `client_id` and `scopes` → `{device_code, user_code, verification_uri, expires_in, interval}`.
2. Dock shows the `user_code` and a "Open twitch.tv/activate" button (clipboard-copy also provided; per the no-focus-steal rule the plugin shows the link rather than force-launching a browser).
3. Plugin polls `POST https://id.twitch.tv/oauth2/token` with `grant_type=urn:ietf:params:oauth:grant-type:device_code` at the returned `interval` until success/expiry.
4. Response includes `access_token` **and** `refresh_token`. Refresh via `POST /oauth2/token` with `grant_type=refresh_token&client_id=...` — no secret required for public clients.

**No server anywhere.** No localhost listener, no hosted callback, no shipped secret. The `client_id` is public by design for public clients. This is the only flow that satisfies local-first + refresh + zero infrastructure simultaneously.

### 3.2 Scopes

Exactly one: **`channel:manage:broadcast`** — covers Modify Channel Information (category + title) *and* Create Stream Marker (Twitch API reference). Get Channel Information and Search Categories need no extra scope. Requesting the minimum keeps the consent screen unscary.

### 3.3 Token storage, validation, refresh, expiry

- Store `{access_token, refresh_token, user_id, login, obtained_at, expires_in}` in a separate file `tokens.bin` under the plugin config dir, encrypted with **DPAPI** (`CryptProtectData` with `CRYPTPROTECT_UI_FORBIDDEN`, Microsoft Learn) — machine+user bound, no key management. Never in `config.json`, never anywhere a scene-collection export could carry it.
- **Hourly validation:** Twitch's docs require apps to validate tokens via `GET https://id.twitch.tv/oauth2/validate` on startup and hourly; comply (it also returns `user_id`, our `broadcaster_id`).
- **Proactive refresh** at 75% of `expires_in`.
- **Mid-stream expiry/401:** on any 401, attempt one refresh and retry the request once. If refresh fails (token revoked / password change): freeze automation, keep the last category untouched, set the dock status to "Reconnect to Twitch" with the DCG button, and emit a non-modal OBS log line. Never interrupt streaming, never pop a modal during a live show.

### 3.4 Endpoints used and rate limits

- `PATCH /helix/channels?broadcaster_id=` body `{"game_id": "..."}` (+ `title` when templating is on) — the core action.
- `GET /helix/channels?broadcaster_id=` — initial state sync.
- `GET /helix/games?name=` and `GET /helix/search/categories?query=` — mapping tier 3.
- `POST /helix/streams/markers` — Section 5 feature.
- Rate limits (Twitch API guide): token-bucket, 800 points/min per client-user token pair, 1 point per request. Worst realistic usage here is <10 requests/min; still, respect `Ratelimit-Remaining`/`Ratelimit-Reset` headers and back off on 429. Serialize all Helix writes through one queue so retries can't reorder, and enforce `MIN_PATCH_SPACING_S` between category PATCHes.
- **Open question — parallel research (do not block on it):** exact `PATCH /helix/channels` limits, whether *frequent category changes* carry any non-rate-limit penalty (directory placement, discoverability, viewer-visible churn in the channel page/notifications), and Restream interaction are being investigated separately; findings land in `Working\SignalBox\rate-limits.md`. This spec is robust to any answer because every frequency-related behaviour is governed by the named constants in Section 2.1 — tuning is a config-defaults change, not a redesign. Until findings land, the defaults are deliberately conservative (≤ a few PATCHes per hour in normal use; flap breaker caps pathological cases).

### 3.5 External category writers (Restream, dashboards, mods) — coexistence strategy

A streamer multistreaming via Restream (or a mod using the Twitch dashboard, or another tool) can set the channel category *after* we do. If something silently overwrites us, this plugin looks broken; if we blindly re-PATCH, we fight the other writer in a loop. Strategy: **detect external writes and yield by default.**

- While live, every `CHANNEL_SYNC_INTERVAL_S` (default 120 s) — and immediately before any PATCH — `GET /helix/channels` and compare `game_id` against the last value *we* set.
- Mismatch ⇒ an external actor changed it. Default response: **suspend automation** (state `EXTERNAL_OVERRIDE`), keep hands off, dock notice: "Category was changed outside SignalBox — automation paused. [Resume]". Rationale: the external writer is either the streamer's own intent (dashboard, mod) or a tool the streamer configured (Restream); both outrank us. Never enter a write war.
- Optional "enforce my category" mode (off by default) re-asserts after a confirmation delay — provided for users who *want* the plugin to win over a misconfigured sync, with a warning that running it alongside Restream's own category sync will cause visible churn.
- Documentation ships a **known-interactions note**: if you multistream through Restream with its title/category sync enabled, disable that sync or run SignalBox in paused mode — two automatic writers on one field cannot both be right. This is a documented incompatibility with a graceful default, not an undefined behaviour.
- The same mechanism doubles as the manual-change guard: a streamer changing category by hand mid-stream automatically pauses us until they opt back in. Cost: ~30 GETs/hour, far inside any plausible limit.

---

## 4. Architecture

### 4.1 Toolchain and OBS surface

- **Base:** official `obs-plugintemplate` (github.com/obsproject/obs-plugintemplate): CMake ≥3.28 presets, C++17, GitHub Actions CI for Windows. OBS ≥ 28 uses **Qt 6** for frontend plugins; target **minimum OBS 30.x** so the dock can use `obs_frontend_add_dock_by_id()` (introduced OBS 30; the older `obs_frontend_add_dock()` is deprecated). Verify against the template's pinned OBS version at implementation time.
- **Frontend events:** `obs_frontend_add_event_callback` for `OBS_FRONTEND_EVENT_STREAMING_STARTED / STOPPED / EXIT / SCENE_COLLECTION_CHANGED`.
- **HTTP:** libcurl (`twitch/HttpTransport`) — the same libcurl OBS itself ships (`bin/64bit/libcurl.dll`) and uses for its own HTTPS traffic, resolved at runtime against OBS's already-loaded copy rather than bundled. **Not** `QNetworkAccessManager`: OBS's Qt distribution carries no TLS backend plugin (Qt is UI-only there), so QNAM cannot do HTTPS at all in an OBS process — confirmed live ("No functional TLS backend was found"). libcurl's easy interface is blocking, so HttpTransport runs it on a small dedicated worker thread and always delivers the result back to the calling (Qt main) thread; from every caller's perspective this is still one async callback per request, same shape QNAM gave via `QNetworkReply::finished`.
- **JSON:** nlohmann/json (vendored single header). **Localization:** standard `data/locale/en-US.ini` + `obs_module_text`.

### 4.2 Modules (keep it small — 7 units)

```
src/
  plugin-main.cpp        // obs_module_load/unload; wires everything; registers dock
  DetectionEngine.{h,cpp} // worker thread: poll loop + Section 2 state machine
  InstallIndex.{h,cpp}    // owns providers; provider impls in providers/*.cpp
  ProcessScanner.{h,cpp}  // Toolhelp + OpenProcess + QueryFullProcessImageNameW wrapper
  CategoryResolver.{h,cpp}// 3-tier mapping + learned cache
  TwitchClient.{h,cpp}    // DCG auth, DPAPI token store, Helix calls, rate-limit queue
  CategoryDock.{h,cpp}    // QWidget dock (status, pending countdown, lock, log+undo, auth)
data/ helpers.json  aliases.json  locale/en-US.ini
```

### 4.3 Threading

- **Worker thread** (std::thread, owned by DetectionEngine, condition-variable wakeups every 5 s, joined on `obs_module_unload` with a stop flag): all registry/file I/O (index rebuilds) and process scanning. Never touches OBS or Qt objects.
- **HttpTransport's own worker thread** (one per TwitchAuth/TwitchClient instance, same std::thread + mutex/condition-variable pattern as above): the actual blocking `curl_easy_perform()` call. Touches nothing but libcurl - no OBS, no Qt objects, no shared state beyond its own request queue. Every result crosses back to the Qt main thread via `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` before any callback runs.
- **Qt main thread:** dock UI, all HttpTransport::send() calls and their callbacks, all Helix decisions' *execution*.
- **Handoff:** DetectionEngine posts immutable `DetectionResult` snapshots to a coordinator QObject via `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`. No shared mutable state between threads except an atomic stop flag and a mutex-guarded copy of the install index.
- OBS's UI thread is never blocked: no synchronous network, no synchronous registry scans on it, and dock updates are trivial widget sets.

### 4.4 State and configuration

- **Config:** JSON at `obs_module_config_path("config.json")` (resolves under `%APPDATA%\obs-studio\plugin_config\<module>\`), read/written via `obs_data` (gives atomic safe-save). Contents: poll/confirm/grace timings, only-while-live flag, fallback-category option, lock default, user overrides map, learned mappings cache, feature toggles. Human-readable and hand-editable by design.
- **Tokens:** `tokens.bin` (DPAPI), same directory, never in config.json.
- **In-memory state:** current state-machine state + last-known channel info live in DetectionEngine/coordinator; nothing plugin-critical is stored in scene collections (so profile/collection switching can't corrupt or leak anything).

---

## 5. Candidate feature evaluation

Scores 1–5; rank = (value × distinctiveness) / cost.

| Feature | Value | Distinct. | Cost | Rank score | Verdict |
|---|---|---|---|---|---|
| **Stream markers on category change** | 4 | 5 | 1 | **20.0** | **Build in v1.0. The single best feature.** |
| Per-game user overrides | 5 | 2 | 1 | 10.0 | Not optional — it's the correctness backstop of the resolver. Core, v1.0. |
| Non-game category detection | 4 | 4 | 2 | 8.0 | v1.1. Opt-in profile table (`Photoshop.exe`→Art, `Ableton*.exe`→Music, `Code.exe`→Software & Game Development). Apps need stricter confirmation (60 s + foreground) and games always outrank apps. Huge audience expansion (Just Chatting/creative streamers), but ship after core detection has proven trust. |
| Stream title templating (`{game}` token) | 3 | 2 | 1 | 6.0 | v1.1. Same PATCH call, low cost. Must snapshot the user's original title and restore on disable — title clobbering is the trust risk. |
| Session summary (what was played, how long) | 2 | 2 | 2 | 2.0 | Defer indefinitely. The state machine's log gives it nearly free *later*; adds no live value now. |
| OBS scene switching on game change | 3 | 1 | 3 | 1.0 | **Do not build.** Advanced Scene Switcher owns this space comprehensively; duplicating it poorly dilutes the plugin's identity and doubles the blast radius of a false detection (wrong category *and* wrong scene live). |
| Multi-platform (Kick, YouTube) | 2 | 2 | 5 | 0.8 | **Do not build now.** YouTube has no equivalent live category granularity; Kick's API maturity/auth story is weak. Revisit only on demonstrated demand. The detection core is platform-agnostic by design, so the door stays open. |

**Why markers win:** on every confirmed category change while live, `POST /helix/streams/markers` with description "Now playing: <game>". The VOD becomes chaptered by game with zero streamer input — genuinely useful for editors and clip-hunting, essentially no other plugin does it, it needs **no new scope** (`channel:manage:broadcast` already covers it) and no new auth/UI/threading — roughly thirty lines riding entirely on machinery that must exist anyway. Highest value-per-cost on the board. Caveat handled: markers require the broadcaster to be live with VODs enabled; treat failures as silent-with-log.

**Added feature (own suggestion), v1.0:** the **dock activity log with one-click Undo** (Section 2). It converts the scariest failure mode — wrong category live — into a 2-second recovery, and it's the cheapest trust feature available.

## 6. Deliberately not building

- **Scene/scene-collection switching** — ceded to Advanced Scene Switcher.
- **Any hosted backend** — no relay, no token exchange server, no telemetry endpoint. Local-first is a feature.
- **Anything requiring `PROCESS_VM_READ`, module enumeration, window-title scraping, or injection** — anti-cheat suicide and unnecessary.
- **Screenshot/OCR/ML game recognition** — enormous cost, capture-pipeline entanglement, worse accuracy than metadata.
- **Chat bot / alerts / overlays / "going live" posts** — different products; Twitch ecosystem is full of them.
- **Settings cloud sync, analytics dashboards, IGDB integration** (IGDB needs a client secret → backend → no).
- **Linux/macOS in v1.0** — the detection layer is Win32-specific by design; the module boundary (ProcessScanner + providers) is where ports plug in later.
- **Kick/YouTube in v1.0** — above.

## 7. Risks

1. **False category set live (top risk — reputational, borne by the user).** Mitigations are the spine of this design: metadata-driven identity, 15 s confirmation, unmapped-means-no-action, redundancy guard, the asymmetric 120 s crash-grace before fallback, the flap breaker, external-writer yield, lock toggle, log + Undo. Residual risk accepted and surfaced in the dock ("pending: Elden Ring in 10 s" with a cancel).
2. **Anti-cheat interaction.** Reading image paths with QUERY_LIMITED handles is the documented benign pattern; the plugin never opens invasive handles. Residual risk: a future AC driver blocking even QUERY_LIMITED → detection degrades to "unknown process", never crashes; Steam `RunningAppID` still corroborates.
3. **Twitch API/OAuth policy churn.** DCG is Twitch's own recommended public-client flow; scope or endpoint changes are release-fixable — which is exactly the maintained-plugin promise. Keep TwitchClient isolated so churn stays in one file.
4. **OBS/Qt churn.** Track `obs-plugintemplate` releases; pin minimum OBS 30; CI builds against current + previous OBS. The frontend dock API is the historically churniest surface — CategoryDock is deliberately thin.
5. **Store format drift** (Valve/Epic changing manifest layouts): parsers are tolerant (unknown keys ignored), providers fail independently, and a broken provider degrades that store's detection only. Data-file denylists/aliases mean most fixes are data PRs.
6. **Elevation mismatch:** if a game runs elevated and OBS doesn't, `OpenProcess` may fail → that game undetectable; document it, suggest running the game non-elevated, never suggest elevating OBS.

## 8. Versioning, patch notes, and update communication

The project's stated reason to exist is that it is *actually maintained*. That claim has to be structurally visible, not asserted:

- **Versioning:** Semantic Versioning (`MAJOR.MINOR.PATCH`). MAJOR = config/behaviour breaks (expect ~never for a utility plugin), MINOR = features, PATCH = fixes and data-file updates (aliases/helpers refreshes are legitimate PATCH releases — cheap cadence proof that maintenance is real). Untested builds ship as GitHub **pre-releases** and are promoted after a live soak test; old releases remain downloadable.
- **CHANGELOG:** a repo-root `CHANGELOG.md` in Keep a Changelog format (Added/Changed/Fixed/Data, dated, newest first) is the single source of truth. GitHub Releases bodies are generated from it; the public patch-notes page is the same file rendered via GitHub Pages — one document, three surfaces, zero drift. The dock footer shows the installed version as a link to that page.
- **"Update available" surfacing — opt-in and user-initiated only.** The owner's projects are local-first: an update *check* is an outbound network call, so the plugin performs **no automatic check on launch, ever**. Instead the dock has a "Check for updates" button; clicking it calls the public GitHub Releases API (`GET api.github.com/repos/<owner>/<repo>/releases/latest`, unauthenticated), compares semver against the installed version, and shows the result inline with a link to the release page. Nothing is sent beyond the HTTP request itself, nothing is stored, nothing phones home unprompted. (A "check when I click" model also sidesteps GitHub's unauthenticated 60-req/hour limit trivially.) No auto-download, no auto-install — OBS plugin installs are a user action and should stay one.
- **Issue hygiene as product surface:** issue templates (bug report requires OBS log + plugin version + store platform; unmapped-game report is a dedicated template that feeds `aliases.json` PRs). The alias/helper data files being community-patchable is the maintenance promise made concrete.

## 9. Milestones

- **M1:** ProcessScanner + Steam/Epic providers + matching; dock shows detection only (no Twitch). Validates the core promise safely.
- **M2:** DCG auth + resolver + PATCH + state machine + log/Undo. Feature-complete core.
- **M3 (v1.0):** GOG/Ubisoft/Generic providers, markers, aliases/helpers data files, CI, docs. Cut as GitHub pre-release; promote after live-stream soak test.
- **v1.1:** non-game profiles, title templating, Xbox provider.

## 10. Primary sources

- OBS: `obsproject.com/docs` (plugin API, frontend API), `github.com/obsproject/obs-plugintemplate`.
- Win32 (Microsoft Learn): Process Security and Access Rights; `OpenProcess`; `QueryFullProcessImageNameW`; Tool Help snapshots; `GetForegroundWindow`/`GetWindowThreadProcessId`; `GetProcessMemoryInfo`; `CryptProtectData`; Uninstall Registry Key; `GetPackageFamilyName`.
- Valve Developer Community wiki: KeyValues format; `libraryfolders.vdf`; app manifest (`appmanifest_*.acf`) keys; Steamworks `_CommonRedist` convention.
- Epic: on-disk `.item` manifest JSON under `%PROGRAMDATA%\Epic\EpicGamesLauncher\Data\Manifests` (publicly documented format).
- Twitch: `dev.twitch.tv/docs/authentication` (Device Code Grant, refresh, validate), `dev.twitch.tv/docs/api/reference` (Get/Modify Channel Information, Get Games, Search Categories, Create Stream Marker), API guide rate-limit section.

---

# ADDENDUM — The Prompt (owner decision, 2026-08-18)

This supersedes the crash-fallback default debated above. **Neither
"revert to Just Chatting" nor "hold the last category" is the default.
The plugin asks.**

## Why this resolves the argument

The hold-vs-revert debate was an attempt to guess correctly on the user's
behalf in a genuinely ambiguous moment. Both guesses are wrong sometimes,
and the cost of a wrong guess is paid publicly by the streamer. Asking
removes the guess at exactly the moment the intent is unknowable — and
costs the user nothing if they ignore it.

## Two triggers, one prompt shape

**Trigger A — go-live mismatch.** On stream start, if the live category
does not match what is running:

> **You're live in "Just Chatting" but Path of Exile 2 is running.**
> `Set to Path of Exile 2` · `Keep Just Chatting` · `Don't ask this stream`

**This is the product's founding use case solved directly** — forgetting to
set the category before going live. It also delivers value to users who
would never enable automatic switching at all, which materially widens the
audience.

**Trigger B — game closed while live.** After the grace period, if no game
is detected and the stream is still live:

> **No game detected. You're still live in "Path of Exile 2".**
> `Wait for a new game` · `Switch to Just Chatting` · `Ignore this change`

The three options map to real, distinct intents the owner identified:
- **Wait** — about to launch something else
- **Just Chatting** — ending monologue, or genuinely done playing
- **Ignore** — game is updating/restarting and will return to the same
  category shortly

## Hard constraints

1. **NEVER MODAL. NEVER STEALS FOCUS.** The user is likely full-screen in a
   game, live. A focus-stealing dialog could tab them out mid-fight — worse
   than any wrong category this feature exists to prevent. Surface it as
   dock state plus, at most, a passive toast. Visible on a glance at OBS,
   invisible otherwise.
2. **TIMEOUT TO THE SAFE DEFAULT.** No answer within the timeout → **hold
   the last category**, the least destructive outcome. Ignoring the prompt
   must always be safe, because ignoring it is the most likely response.
3. **"IGNORE" NEEDS A SCOPE.** The button means *ignore this one*, with a
   "don't ask again this stream" checkbox alongside. The update-restart
   scenario repeats several times per session, and asking every time is the
   nagging this design exists to avoid.
4. **ONLY WHEN LIVE.** No prompts when not streaming — there is nothing to
   get wrong.

## Positioning

**It doesn't take the wheel, it taps you on the shoulder.**

Every competitor's model is automation the user must trust blindly, and
that trust is exactly what a false detection destroys. A tool that notices
and defers is a different product, and it is defensible on the axis the
competition cannot follow without abandoning their own premise.

## Restream interaction (from `rate-limits.md`)

Restream manages its own stream metadata and **can overwrite a category set
directly on Twitch** — the plugin would appear broken through no fault of
its own. Detect Restream usage and either stand down or warn plainly. A
clear "Restream detected, standing down" message is far better than losing
an invisible fight and taking the blame.

## Timing constants — keep these separate and named

- **Confirmation before acting:** ~15s (three consecutive 5s polls)
- **Grace period after a game disappears:** ~120s (survives crash-restarts)
- **Minimum interval between category changes:** 30–60s
- **Prompt timeout before falling through to hold:** to be tuned

These are four distinct concerns. Do not collapse them into one value.

---

# RESTREAM — REVISED (owner first-hand observation, 2026-08-18)

**This supersedes the "detect Restream and stand down" recommendation
above.** That came from inference off a Twitch developer-forum thread. The
owner is an actual Restream user and reports different observed behaviour,
which is the better evidence.

## Observed behaviour

- Restream **can** set title and its own category.
- Restream's category applies at the **initial setting / stream start only**.
- **Once live, Restream does NOT apply category changes.** Changing it
  appears only to work while the stream is off.

## Why this is good news

If it holds, Restream and this plugin are **complementary, not
conflicting**:

| | Owns |
|---|---|
| Restream | The category at go-live |
| SignalBox | Every change while live |

No contention, no standing down, and **"works alongside Restream" becomes a
selling point** rather than a documented incompatibility. Restream users are
a large share of the streaming population.

It also makes **Trigger A (the go-live mismatch prompt) more valuable for
Restream users specifically** — Restream's stored category is among the most
likely things to be stale, because it is set once in a settings page and
then forgotten. That is exactly the founding problem this plugin exists for.

## MUST VERIFY BEFORE RELYING ON IT

The observation is "from what I can see", not a deliberate test. The failure
mode would be **intermittent and very hard to diagnose from a user report**:
the category reverts minutes later with no obvious cause.

**Test to run:** go live, let the plugin change the category, leave it
10-15 minutes, then open the Restream dashboard and modify/save something.
Also worth probing: a platform reconnect, and a dropped-then-resumed ingest.
If the category survives all of those, the division of responsibility above
is safe to build on.

**Until verified**, build the Restream-detection hook but leave its default
behaviour as "proceed normally" rather than "stand down" — the hook costs
little and gives a place to put a warning if the test fails.
