/*
 * SignalBox - core/CaptureRecovery.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * "Capture card recovery": restarts a Video Capture Device source whose
 * picture has gone black (HDMI signal dropped, card fell off USB 3, ...).
 * A restart is what the source's Deactivate/Activate button does: its
 * "active" setting goes false, then true again.
 *
 * Polls once a second from a Qt timer on the main thread - a few property
 * reads, and nothing at all while the feature is off. Settings are read
 * from PluginConfig on every tick, so a change in the settings dialog
 * applies without any extra wiring. The same restart is bound to the OBS
 * hotkey "SignalBox: Restart capture card".
 *
 * THREADING: Qt main thread only. The OBS hotkey callback (hotkey thread)
 * only queues restartNow() onto this object.
 */

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include "CaptureRecoveryPolicy.h"
#include "PluginConfig.h"

class QTimer;

namespace signalbox::core {

class CaptureRecovery : public QObject {
	Q_OBJECT

public:
	explicit CaptureRecovery(PluginConfig &config, QObject *parent = nullptr);
	~CaptureRecovery() override;

	// Names of the scene collection's Video Capture Device sources.
	static QStringList captureSourceNames();

	// Process-wide instance for the settings dialog's button; may be null.
	static CaptureRecovery *instance();

public slots:
	// Restarts the saved source with the saved reset command.
	void restartNow();
	// Restarts a specific source (the settings dialog passes what is on screen).
	void restartSource(const QString &sourceName, const QString &resetCommand);

private:
	void onTick();
	void finishRestart(const QString &sourceName);

	PluginConfig &config_;
	QTimer *timer_ = nullptr;
	CaptureRecoveryPolicy policy_;
	bool restarting_ = false;
	size_t hotkeyId_ = static_cast<size_t>(-1); // obs_hotkey_id; -1 = none.
	QString lastSourceNote_;
};

} // namespace signalbox::core
