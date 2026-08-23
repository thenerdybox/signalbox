/*
 * SignalBox - core/plugin-main.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Plugin entry point: obs_module_load/unload, dock registration, and
 * DetectionEngine lifecycle. This is the one file allowed to know about
 * both the OBS module API and the plugin's own core types - everything
 * downstream (detection/, twitch/, ui/) should not need to reach back
 * into obs-module.h or obs-frontend-api.h directly except where their own
 * documented interface says so (PluginConfig's obs_data usage, TokenStore's
 * DPAPI/file I/O, etc.).
 *
 * WIRING (this is the file that closes the loop DESIGN.md 4.3 describes):
 *   DetectionEngine (worker thread)
 *     -> QMetaObject::invokeMethod(dock, ..., Qt::QueuedConnection)
 *     -> CategoryDock::onTrackedProcessExited() / onDetectionResult()
 *        (Qt main thread)
 *     -> DetectionStateMachine (owned by CategoryDock)
 * Only an immutable DetectionResult snapshot crosses the thread boundary -
 * see DetectionEngine.h's threading-boundary comment. No obs_* or Qt
 * object is ever touched from the worker thread's callback body itself;
 * the callback's only job is to hand the snapshot to invokeMethod().
 *
 * This file also resolves every OBS-aware path (obs_module_file for
 * bundled data/ files, obs_module_config_path for the writable config
 * dir) that DetectionEngine, HelperDenylist, and CategoryResolver need but
 * cannot resolve themselves - see their own OWNERSHIP NOTE / threading-
 * boundary comments for why that split exists.
 */

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <QMetaObject>
#include <QString>

#include "../plugin-support.h"
#include "signalbox-build-id.h"
#include "../twitch/TwitchClientId.h"
#include "../ui/CategoryDock.h"
#include "CategoryResolver.h"
#include "DetectionEngine.h"
#include "PluginConfig.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

signalbox::ui::CategoryDock *g_dock = nullptr;
signalbox::core::DetectionEngine *g_engine = nullptr;
signalbox::core::PluginConfig *g_config = nullptr;
signalbox::core::CategoryResolver *g_resolver = nullptr;

// bmalloc'd UTF-8 path -> std::wstring, freeing the bmalloc'd
// intermediate. Same conversion pattern already used by TokenStore.cpp/
// TwitchClient.cpp for obs_module_config_path()'s return value.
std::wstring Utf8PathToWide(char *utf8Path)
{
	if (!utf8Path) {
		return {};
	}
	std::wstring result;
	wchar_t *widePath = nullptr;
	if (os_utf8_to_wcs_ptr(utf8Path, 0, &widePath) > 0 && widePath) {
		result = widePath;
	}
	if (widePath) {
		bfree(widePath);
	}
	bfree(utf8Path);
	return result;
}

// data/<name> - bundled, read-only resource shipped with the plugin
// (helpers.json, aliases.json). Empty if the module's data path can't be
// resolved (e.g. a malformed install) - every consumer of this degrades
// gracefully to a compiled-in fallback or "tier contributes nothing"
// rather than failing (see HelperDenylist::LoadFromFile,
// CategoryResolver::loadAliasTable).
std::wstring ResolveDataFile(const char *name)
{
	return Utf8PathToWide(obs_module_file(name));
}

// <config dir>/<name> - writable, per-install state (the index cache).
// Ensures the config directory exists first (obs_module_config_path only
// builds the path string - see TokenStore.cpp's own comment on this).
std::wstring ResolveConfigFile(const char *name)
{
	char *dirUtf8 = obs_module_config_path("");
	if (dirUtf8) {
		os_mkdirs(dirUtf8); // MKDIR_EXISTS is a normal, expected return here.
		bfree(dirUtf8);
	}
	return Utf8PathToWide(obs_module_config_path(name));
}

// Second, independent OBS_FRONTEND_EVENT_STREAMING_STARTED/_STOPPED
// listener (CategoryDock already registers its own, for
// DetectionStateMachine::setLive() - see CategoryDock.cpp). Threads live
// state into DetectionEngine for adaptive polling (project brief item 4).
// Kept separate rather than reaching into CategoryDock for this, per the
// file-ownership boundary: this stays a plugin-main.cpp/core concern,
// src/ui is untouched for it.
//
// ALSO handles OBS_FRONTEND_EVENT_EXIT (see the project report's shutdown-
// ordering fix): OBS destroys docks - Qt child widgets of the main window -
// as part of main-window teardown, and that teardown is not documented (or,
// empirically, guaranteed) to happen AFTER obs_module_unload() runs. If the
// worker thread is still polling when dock destruction starts, the
// resultCallback_ lambda below can call QMetaObject::invokeMethod() on an
// already-destroyed QObject - undefined behavior, observed as an
// intermittent crash on exit. Stopping (and joining) the engine HERE, while
// this frontend event fires with the main window and dock still guaranteed
// alive, closes that window: the worker thread is guaranteed dead before
// any dock destruction can begin. obs_module_unload()'s own g_engine->stop()
// remains as the fallback for any exit path that skips frontend events
// (e.g. a hard process kill) - calling stop()/joining twice is safe
// (std::thread::join() is only attempted while joinable()) and g_dock is
// only ever cleared to nullptr, never deleted, from either place (OBS owns
// the actual widget - see obs_module_unload()'s own note on this).
void LiveStateTrampoline(enum obs_frontend_event event, void * /*privateData*/)
{
	if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED) {
		if (g_engine) {
			g_engine->setLive(true);
		}
	} else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED) {
		if (g_engine) {
			g_engine->setLive(false);
		}
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		if (g_engine) {
			g_engine->stop(); // Joins the worker thread - see comment above.
		}
		g_dock = nullptr; // See obs_module_unload()'s matching note; not owned, never deleted here.
	}
}

} // namespace

bool obs_module_load(void)
{
	// The build id (see CMakeLists.txt's BUILD IDENTITY block) is logged
	// beside the version because this line is the first thing anyone reads
	// when a fix appears not to have worked, and "which build is actually
	// loaded" is the first question worth ruling out. A trailing "+" means
	// the DLL was compiled from a working tree with uncommitted changes.
	obs_log(LOG_INFO, "plugin loaded successfully (version %s, build %s)", PLUGIN_VERSION, SIGNALBOX_BUILD_ID);

	// --- Config: loaded once here, and this is now the ONLY PluginConfig
	// instance for the whole plugin - CategoryDock takes a reference to
	// it (see its constructor) rather than loading a second copy, so
	// Settings > Save actually persists what DetectionEngine/
	// DetectionStateMachine/CategoryResolver are running with. ---
	g_config = new signalbox::core::PluginConfig();
	g_config->load();

	// --- CategoryResolver: tiers 1+2 live from construction (user
	// overrides via *g_config, the bundled alias table below); tier 3
	// (Twitch Helix) is attached later, once Twitch auth succeeds, via
	// CategoryResolver::setLookup() - see CategoryDock::attachTwitchClient().
	// Constructed before g_dock because CategoryDock's constructor takes
	// a reference to this resolver (it owns the coordinator that
	// actually calls resolve()). ---
	g_resolver = new signalbox::core::CategoryResolver(*g_config);
	g_resolver->loadAliasTable(ResolveDataFile("aliases.json"));

	// --- Dock registration: obs_frontend_add_dock_by_id() is OBS 30+
	// only (DESIGN.md 4.1). OBS takes ownership of the widget once
	// registered; this plugin must not delete it in obs_module_unload(). ---
	g_dock = new signalbox::ui::CategoryDock(*g_config, *g_resolver);
	if (!obs_frontend_add_dock_by_id("signalbox-dock", "SignalBox", g_dock)) {
		obs_log(LOG_WARNING, "failed to register dock - obs_frontend_add_dock_by_id returned false "
				     "(requires OBS 30+)");
	}

	// Twitch client id: safe to pass through unconditionally even while
	// empty - CategoryDock::setTwitchClientId() no-ops on an empty
	// string and leaves the "Connect to Twitch" button disabled (see its
	// own doc comment). See src/twitch/TwitchClientId.h for how to set a
	// real one.
	g_dock->setTwitchClientId(QString::fromUtf8(signalbox::twitch::twitchClientId().data(),
						      static_cast<int>(signalbox::twitch::twitchClientId().size())));

	// --- DetectionEngine: worker thread start. ---
	signalbox::core::TimingConstants engineTiming = g_config->timing();

	g_engine = new signalbox::core::DetectionEngine(
		[](signalbox::core::DetectionResult result) {
			// WORKER THREAD. Only the immutable `result` snapshot may
			// cross the boundary - see DetectionEngine.h's threading
			// boundary comment. g_dock is safe to read here without
			// synchronization: it is written once before g_engine->start()
			// (happens-before the worker thread's first iteration, via
			// std::thread's constructor) and only ever cleared to nullptr
			// in obs_module_unload() AFTER g_engine->stop() has joined
			// this thread (happens-after every access here) - see
			// obs_module_unload() below.
			signalbox::ui::CategoryDock *dock = g_dock;
			if (!dock) {
				return;
			}
			QMetaObject::invokeMethod(
				dock,
				[dock, result]() {
					// Qt main thread. Apply the exit signal (if any)
					// before the fresh poll result, so GRACE sees the
					// exit even when a fast relaunch also produced a
					// winning candidate in the same poll (DESIGN.md
					// 2.2 - "exit observed within one poll").
					if (result.trackedExit) {
						dock->onTrackedProcessExited(result.trackedExit->reason,
									      result.trackedExit->game);
					}
					if (result.indexRebuild) {
						dock->onIndexRebuilt(result.indexRebuild->gameCount,
								      result.indexRebuild->excludedCount,
								      result.indexRebuild->duplicateCount,
								      result.indexRebuild->durationMs,
								      result.indexRebuild->userRequested,
								      result.indexRebuild->denylistRuleCount);
					}
					dock->onDetectionResult(result.detected, result.indexedGameCount);
				},
				Qt::QueuedConnection);
		},
		engineTiming);

	g_engine->registerDefaultProviders();
	g_engine->setHelperDenylistPath(ResolveDataFile("helpers.json"));
	g_engine->setIndexCachePath(ResolveConfigFile("install-index-cache.json"));
	g_engine->start();

	// Dock "Rescan" button -> DetectionEngine::requestRescan() (project
	// brief item 6: this had zero callers). Connected here, once both
	// g_dock and g_engine exist, per the file-ownership split
	// (CategoryDock does not reach into DetectionEngine directly - see
	// CategoryDock.h's rescanRequested() doc comment).
	//
	// ALSO reloads the alias table (data/aliases.json) right here,
	// synchronously, rather than threading it through DetectionEngine at
	// all: CategoryResolver is main-thread-only (see its THREADING note),
	// this lambda already runs on the Qt main thread (rescanRequested() is
	// emitted by the dock itself, also main-thread-only), and the whole
	// point of this change is that editing aliases.json/helpers.json used
	// to require a full OBS restart to take effect. The denylist half of
	// that fix lives on DetectionEngine's worker thread (see its
	// workerLoop() - rescanRequested_ triggers a fresh
	// HelperDenylist::LoadFromFile() there); this is the other half.
	QObject::connect(g_dock, &signalbox::ui::CategoryDock::rescanRequested, g_dock, []() {
		if (g_resolver) {
			g_resolver->loadAliasTable(ResolveDataFile("aliases.json"));
			const std::size_t aliasCount = g_resolver->aliasCount();
			obs_log(LOG_INFO, "rescan: reloaded alias table (%zu entries)", aliasCount);
			// Also say so in the dock's activity log, not only the OBS
			// log file - the activity log is the one place a streamer
			// looks to understand what this plugin just did, and a
			// reload nobody can see is indistinguishable from one that
			// never happened. Safe to call directly: this lambda is
			// already on the Qt main thread (see above), which is the
			// only thread CategoryDock may be touched from.
			if (g_dock) {
				g_dock->onAliasTableReloaded(aliasCount);
			}
		}
		if (g_engine) {
			g_engine->requestRescan();
		}
	});

	obs_frontend_add_event_callback(&LiveStateTrampoline, nullptr);

	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(&LiveStateTrampoline, nullptr);

	if (g_engine) {
		// Normally already stopped/joined by LiveStateTrampoline's
		// OBS_FRONTEND_EVENT_EXIT handler above (see its comment) - this
		// is the fallback for any unload path that skips frontend events.
		// stop() is idempotent (join() only runs while joinable()).
		g_engine->stop(); // Joins the worker thread; must complete before we return.
		delete g_engine;
		g_engine = nullptr;
	}

	// g_dock is owned by OBS's frontend once registered above - do not
	// delete it here. Clearing the pointer only after the worker thread
	// has joined (see above) is what makes the ResultCallback's
	// unsynchronized read of g_dock safe - see that lambda's comment.
	// Usually already nullptr by now (set by the EXIT handler above).
	g_dock = nullptr;

	// g_resolver/g_config are DELIBERATELY LEAKED here, not deleted.
	// CategoryDock now holds live references to both (see its
	// constructor - the "ONE PluginConfig INSTANCE" fix), and the actual
	// dock widget's Qt-level destruction timing relative to this
	// function is not something OBS's frontend API documents or
	// guarantees - the widget is a Qt child of the main window,
	// reclaimed whenever OBS tears that down, which is not provably
	// before this call returns. Deleting either here risked a
	// use-after-free in a QWidget that might still be alive; leaking two
	// small, module-lifetime singletons a few milliseconds before
	// process exit is the safe trade. Same reasoning g_dock's own
	// comment above already applies to the dock itself.
	obs_log(LOG_INFO, "plugin unloaded");
}
