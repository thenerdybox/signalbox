/*
 * SignalBox - core/CaptureRecovery.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * See CaptureRecovery.h.
 */

#include "CaptureRecovery.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QMetaObject>
#include <QPointer>
#include <QTimer>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include <chrono>

#include "../plugin-support.h"

namespace signalbox::core {

namespace {

constexpr const char *kSourceId = "dshow_input";
constexpr const char *kHotkeyName = "signalbox.restart_capture_card";
constexpr const char *kHotkeySaveKey = "signalbox_restart_capture_card_hotkey";
constexpr int kReactivateDelayMs = 600;
constexpr int kReactivateDelayAfterResetMs = 3000;

CaptureRecovery *g_instance = nullptr;

std::int64_t NowMs()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool SetSourceActive(obs_source_t *source, bool active)
{
	obs_data_t *settings = obs_source_get_settings(source);
	if (!settings) {
		return false;
	}
	obs_data_set_bool(settings, "active", active);
	obs_source_update(source, settings);
	obs_data_release(settings);
	return true;
}

// Runs the user's reset program or shortcut without waiting for it.
void RunResetCommand(const QString &command)
{
#ifdef _WIN32
	const std::wstring path = command.toStdWString();
	const HINSTANCE result = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWMINNOACTIVE);
	if (reinterpret_cast<INT_PTR>(result) <= 32) {
		obs_log(LOG_WARNING, "capture-recovery: could not start the reset command (code %d)",
			static_cast<int>(reinterpret_cast<INT_PTR>(result)));
	}
#else
	Q_UNUSED(command);
#endif
}

void HotkeyCallback(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	// Hotkey thread: only queue the work onto the Qt main thread.
	if (!pressed || !g_instance) {
		return;
	}
	QMetaObject::invokeMethod(g_instance, "restartNow", Qt::QueuedConnection);
}

void SaveCallback(obs_data_t *saveData, bool saving, void *data)
{
	const size_t id = *static_cast<size_t *>(data);
	if (id == static_cast<size_t>(-1)) {
		return;
	}
	if (saving) {
		obs_data_array_t *array = obs_hotkey_save(id);
		obs_data_set_array(saveData, kHotkeySaveKey, array);
		obs_data_array_release(array);
	} else {
		obs_data_array_t *array = obs_data_get_array(saveData, kHotkeySaveKey);
		obs_hotkey_load(id, array);
		obs_data_array_release(array);
	}
}

} // namespace

CaptureRecovery::CaptureRecovery(PluginConfig &config, QObject *parent)
	: QObject(parent), config_(config), policy_(config.captureRecoveryBlackSeconds() * 1000LL)
{
	g_instance = this;
	hotkeyId_ = obs_hotkey_register_frontend(kHotkeyName, "SignalBox: Restart capture card", &HotkeyCallback,
						 nullptr);
	obs_frontend_add_save_callback(&SaveCallback, &hotkeyId_);

	timer_ = new QTimer(this);
	timer_->setInterval(1000);
	connect(timer_, &QTimer::timeout, this, &CaptureRecovery::onTick);
	timer_->start();
}

CaptureRecovery::~CaptureRecovery()
{
	obs_frontend_remove_save_callback(&SaveCallback, &hotkeyId_);
	if (hotkeyId_ != static_cast<size_t>(-1)) {
		obs_hotkey_unregister(hotkeyId_);
	}
	if (g_instance == this) {
		g_instance = nullptr;
	}
}

CaptureRecovery *CaptureRecovery::instance()
{
	return g_instance;
}

QStringList CaptureRecovery::captureSourceNames()
{
	QStringList names;
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			if (qstrcmp(obs_source_get_id(source), kSourceId) == 0) {
				static_cast<QStringList *>(param)->append(QString::fromUtf8(obs_source_get_name(source)));
			}
			return true;
		},
		&names);
	names.sort(Qt::CaseInsensitive);
	return names;
}

void CaptureRecovery::onTick()
{
	if (restarting_ || !config_.captureRecoveryAuto()) {
		policy_.reset();
		return;
	}
	const QString name = QString::fromStdWString(config_.captureRecoverySource());
	if (name.isEmpty()) {
		policy_.reset();
		return;
	}
	obs_source_t *source = obs_get_source_by_name(name.toUtf8().constData());
	if (!source) {
		policy_.reset();
		if (lastSourceNote_ != name) { // Say it once, not every second.
			lastSourceNote_ = name;
			obs_log(LOG_INFO, "capture-recovery: source \"%s\" is not in this scene collection - waiting",
				name.toUtf8().constData());
		}
		return;
	}
	lastSourceNote_.clear();

	bool switchedOn = true;
	if (obs_data_t *settings = obs_source_get_settings(source)) {
		obs_data_set_default_bool(settings, "active", true);
		switchedOn = obs_data_get_bool(settings, "active");
		obs_data_release(settings);
	}
	const bool showing = switchedOn && obs_source_showing(source);
	const bool hasFrames = obs_source_get_width(source) > 0 && obs_source_get_height(source) > 0;
	obs_source_release(source);

	policy_.setBlackAfterMs(config_.captureRecoveryBlackSeconds() * 1000LL);
	if (policy_.tick(NowMs(), showing, hasFrames) == CaptureRecoveryPolicy::Action::Restart) {
		obs_log(LOG_WARNING, "capture-recovery: \"%s\" has had no picture for %d s - restarting it",
			name.toUtf8().constData(), config_.captureRecoveryBlackSeconds());
		restartSource(name, QString::fromStdWString(config_.captureRecoveryResetCommand()));
	}
}

void CaptureRecovery::restartNow()
{
	obs_log(LOG_INFO, "capture-recovery: manual restart requested");
	restartSource(QString::fromStdWString(config_.captureRecoverySource()),
		      QString::fromStdWString(config_.captureRecoveryResetCommand()));
}

void CaptureRecovery::restartSource(const QString &sourceName, const QString &resetCommand)
{
	if (restarting_) {
		return;
	}
	if (sourceName.isEmpty()) {
		obs_log(LOG_WARNING, "capture-recovery: no capture source chosen - nothing to restart");
		return;
	}
	obs_source_t *source = obs_get_source_by_name(sourceName.toUtf8().constData());
	if (!source) {
		obs_log(LOG_WARNING, "capture-recovery: source \"%s\" not found - nothing to restart",
			sourceName.toUtf8().constData());
		return;
	}
	restarting_ = true;
	if (!resetCommand.isEmpty()) {
		obs_log(LOG_INFO, "capture-recovery: running the hardware reset command");
		RunResetCommand(resetCommand);
	}
	SetSourceActive(source, false);
	obs_source_release(source);
	obs_log(LOG_INFO, "capture-recovery: \"%s\" deactivated", sourceName.toUtf8().constData());

	const int delay = resetCommand.isEmpty() ? kReactivateDelayMs : kReactivateDelayAfterResetMs;
	QPointer<CaptureRecovery> self(this);
	QTimer::singleShot(delay, this, [self, sourceName]() {
		if (self) {
			self->finishRestart(sourceName);
		}
	});
}

void CaptureRecovery::finishRestart(const QString &sourceName)
{
	if (obs_source_t *source = obs_get_source_by_name(sourceName.toUtf8().constData())) {
		SetSourceActive(source, true);
		obs_source_release(source);
		obs_log(LOG_INFO, "capture-recovery: \"%s\" activated again", sourceName.toUtf8().constData());
	}
	restarting_ = false;
	policy_.reset();
}

} // namespace signalbox::core
