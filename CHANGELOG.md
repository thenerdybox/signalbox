# Changelog

All notable changes to SignalBox are documented here, in [Keep a
Changelog](https://keepachangelog.com/en/1.1.0/) format. This file is the
single source of truth for release notes — GitHub Releases and the public
patch-notes page are both generated from it.

## [0.2.7] — 2026-10-07

### Added

- **SignalBox now keeps checking your category while you stream.** Every two
  minutes while you are live and a game is running, it reads your Twitch
  category and puts the game's category back if something else changed it - a
  multistream service pushing its own saved category, or an edit made
  elsewhere. It also checks 15 and 60 seconds after you go live, which is when
  those services usually overwrite it. Manual lock, the Stream Ending hold and
  a category you chose yourself are always left alone, and the check never
  runs while you are offline.
- **A question when no game is running while you are live.** If you go live
  with nothing running, or a game closes and nothing replaces it, SignalBox
  asks what is going on: stream ending soon (it stops asking), switch to Just
  Chatting, or waiting for a game to update, load or install (it keeps
  scanning and asks again in two minutes if there is still nothing). A game
  starting at any point dismisses the question and switches normally. The
  question appears in the dock and as a small card on your screen that never
  takes focus away from your game; clicking the card brings up the dock.
- Settings: how often the live category is re-checked, whether to also check
  right after going live, and how long "Waiting for a game" stays quiet.
- **Pick a recent game from the dock.** SignalBox remembers the last five
  categories it set. A "Set category to a recent game..." list in the dock
  switches to one in two clicks, any time. The no-game questions also offer
  your recent games as buttons, so one click sets the category and answers
  the question. The list starts with the games SignalBox has already
  looked up for you, so it is useful straight after updating.

### Changed

- **No more question about a mismatched category when you go live.** If a
  game is running and your channel shows a different category when you go
  live, SignalBox switches it to the game straight away. The question it used
  to ask only appeared in the dock, so it usually went unseen and timed out.
  Turn off automatic switching (manual lock) if you want to keep a different
  category while a game is running.
- Leaving the new no-game question unanswered no longer means "never ask
  again": your category stays as it is and the question comes back later.
  Only clicking "Stream ending soon" stops it.
- The question about switching to a creative app's category now also shows
  the small on-screen card while you are live, so you can see it without
  looking at OBS.
- SignalBox's questions now appear at the top of the dock, and the dock
  scrolls when it is short.
- The log now records the category found at go-live and the result of each
  check.

### Fixed

- **Going live through Restream (or another multistream service) could leave
  your stream in the wrong category for the whole stream.** SignalBox set the
  right category before you went live, the service then replaced it with its
  own saved one at go-live, and if the go-live question went unanswered for 20
  seconds SignalBox kept the wrong category and never checked again. It now
  switches back to the game as soon as it sees the wrong category, and keeps
  checking for the rest of the stream.
- The question about a mismatched category at go-live never appeared anywhere
  you would see it, only in the dock, then timed out with "No response" in
  the log. That question is gone (see above).
- Questions in the dock could be cut off below the bottom edge of a short
  dock, so there was never anything to click and every question timed out.
- **"Twitch reconnect needed" showed every time OBS started, even though
  SignalBox was connected and working.** The saved Twitch login expires after
  a few hours, so it is normally out of date when OBS starts. SignalBox
  renewed it automatically, but the dock kept showing the old warning. It now
  renews the login as OBS starts and the dock says "Connected" once that is
  done. "Reconnect needed" now only appears when you really do have to
  connect again.
- The dock could show "Live category: (unknown)" for a whole session after
  OBS started. It now reads the category again once the Twitch login has been
  renewed.

## [0.2.6] — 2026-08-23

### Added

- **"Check for updates" actually reports something now.** It tells you in the
  activity log whether you are up to date, whether a newer version exists, or
  why the check failed. When there is a newer one, the button turns into
  **Get v0.x.y** and opens that release page.

### Changed

- The version in the dock footer now links to the public release notes at
  thenerdybox.com instead of a page that did not exist.

### Fixed

- The update check pointed at a repository that had not been published, so it
  always failed, and it only ever reported the result in a tooltip on a button
  that looked unchanged — indistinguishable from the button doing nothing.

## [0.2.5] — 2026-08-23

### Changed

- **"Stream Ending" is now "Stream Ending/Stop Detection".** Same button,
  clearer about the second half of what it does.
- It and **Just Chatting** now sit on their own full-width rows directly
  under **Fix This!**, instead of sharing one line. In a narrow dock the
  second button used to get squeezed.
- While the hold is on, the button reads **"Detection stopped — press to
  resume"** rather than appending a state suffix to an already long name.

## [0.2.4] — 2026-08-22

### Added

- **"Stream Ending".** A button next to **Fix This!** that tells SignalBox to
  stop touching your category while you wrap up — outro, chatting, credits,
  whatever. You do not have to remember to turn it back on: it clears itself
  when you stop streaming, or when a new game starts. Opening a browser or an
  editor during the outro does not count as a new game, so it stays held.
  While it is on, the dock says so, and no prompts are raised.
- **"Just Chatting".** A one-click button that sets your Twitch category to
  your fallback category (**Settings → Fallback category**) right now,
  whether you are live or not. Automatic switching carries on as normal
  afterwards.
- **SignalBox now speaks up when nothing is running.** After a couple of
  minutes with no game detected, the dock asks what you are up to and offers
  three answers: set the category to Just Chatting, wait for a game, or
  "just recording" (leave my category alone, and stop asking). It is asked at
  most once per idle stretch — a game running and stopping is what makes it
  a new one — and turning it off permanently is a checkbox in **Settings**.

  This is the first prompt that can appear when you are **not live**, which
  is deliberate: the useful moment to fix a stale category is before you go
  live, not after. It needs Twitch connected, since the whole offer is to set
  a category.

## [0.2.3] — 2026-08-21

### Changed

- **The dock takes up less room.** Status and Twitch now swap instead of
  stacking: before you connect you get **Twitch** and **Activity**, and once
  you're connected the Twitch box disappears and **Status** takes its place.
  If the connection is ever lost, they swap back. Activity, the version, the
  build id, Check for updates, Rescan and Settings are always there.
  Reconnecting stays available any time under **Settings**.

## [0.2.2] — 2026-08-21

### Changed

- When a game can't be matched to a Twitch category, the activity log now
  says **why** — whether Twitch returned nothing for that name, or returned
  something that wasn't a close enough match (and what it was) — and points
  at **Fix This!** to settle it permanently. Previously it only said the
  mapping failed, which looks identical to the plugin being stuck.

## [0.2.1] — 2026-08-21

### Fixed

- The dock offered **Connect to Twitch** directly underneath **Connected as
  \<you\>**. It read as "that didn't work, press me again", and pressing it
  started a fresh authorisation nobody needed. Once connected, the button is
  replaced by a pointer to Settings, where reconnecting still lives for when
  a connection genuinely needs repairing.
- **Open page** stayed on screen after a successful authorisation, offering
  to reopen a page for a code that had already been used.
- Closing a game while offline counted down "asking in Ns" and then asked
  nothing. Holding your category was the correct behaviour — it only asks
  while you are live — but the countdown described something else. It now
  says what it is actually about to do.
- Games whose store name carries ™ or ® could fail to match any Twitch
  category. The name was cleaned up before comparing but not before
  searching, so the search itself went out with the glyphs still in it —
  "STAR WARS™: The Old Republic™" found nothing, while Twitch lists it as
  "Star Wars: The Old Republic".

## [0.2.0] — 2026-08-21

### Added

- **Fix a wrong detection from the dock.** A new **Fix This!** button beside
  the detected app opens a dialog for that exact app: search Twitch and pick
  the category it should always use, tell SignalBox to never switch for it,
  or clear a rule you set earlier. No config files, and nothing to type — the
  app's identity is filled in for you. Your choice applies straight away
  rather than waiting for the app to be detected again.
- **Rescan now reloads `aliases.json` and `helpers.json`.** Editing either
  used to need an OBS restart. Rescan reports what it picked up, so a change
  that was read is distinguishable from one that was not.
- **Build identity.** The dock footer and the OBS log both show which build
  is running, beside the version. Two builds of the same version are no
  longer indistinguishable; a trailing `+` means it was built from
  uncommitted changes.

### Fixed

- A saved per-app rule was written to disk and then ignored by everything
  downstream. Apps without store metadata always asked before switching,
  including ones you had already answered for — so the same question came
  back forever. Your answer is now final.
- **"Never switch for this app"** logged "couldn't map that game to a Twitch
  category" every time that app was detected. It didn't fail to recognise
  anything; you asked for silence, and it now says so.
- **Live category** read "(unknown)" for an entire session until SignalBox
  itself changed something — while connected, with the answer one request
  away. It now syncs as soon as Twitch connects.
- **Settings** asked you to connect to Twitch even when the dock was already
  connected. It shows the connected account and offers "Reconnect" instead.
- A malformed or missing `aliases.json` wiped the entire alias table instead
  of keeping the last good one. With reloading now possible mid-session, one
  stray comma in a hand-edited file could have silently disabled every alias.
- The installer removed only the first older SignalBox it found, and never
  examined the 64-bit registry view, so a duplicate entry could survive an
  update and keep its own copy of the plugin. It now removes every older
  registration and verifies they are gone rather than assuming.

## [Unreleased]

### Added

- Initial project scaffold: CMake/CMakePresets build targeting OBS 30+,
  Qt6, Windows x64, based on `obsproject/obs-plugintemplate`.
- Module interfaces for detection (`IGameProvider`, `ProcessScanner`,
  `InstallIndex`, `DetectedGame`), Twitch integration (`TwitchAuth`,
  `TwitchClient`, `TokenStore`), UI (`CategoryDock`, `PromptWidget`,
  `SettingsDialog`), and core (`PluginConfig`, `TimingConstants`,
  `DetectionStateMachine`, `DetectionEngine`). All stub implementations —
  see `README.md`'s "What's real vs. stubbed".
- `data/helpers.json` and `data/aliases.json` seed files.
- `DESIGN.md` and `rate-limits.md` — full design specification and rate
  limit research backing the timing defaults.
