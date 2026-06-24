/*
 * BlackHoleDock.h — compact OBS dock for the "OBS Audio" virtual microphone.
 *
 * Kept intentionally small: Start/Stop + status, a level meter, and
 * track/gain/mute. The whole thing lives in a scroll area so it resizes to any
 * height. Status polling is throttled (2 s); the level meter only ticks while
 * the mic is actually running.
 */
#pragma once

#include <QWidget>
#include <obs.h>
#include <memory>

class VirtualMicEngine;
class AudioRouterEngine;
class QLabel;
class QPushButton;
class QComboBox;
class QSlider;
class QCheckBox;
class QProgressBar;
class QTimer;

class BlackHoleDock : public QWidget {
	Q_OBJECT
public:
	explicit BlackHoleDock(QWidget *parent = nullptr);
	~BlackHoleDock() override;

	void loadSettings();
	void saveSettings();
	void maybeAutoStart();

public slots:
	// Toggle Start/Stop — also the target of the global OBS hotkey.
	void onToggleMic();

private slots:
	void onGainChanged(int value);
	void onMuteToggled(bool muted);
	void onNoiseToggled(bool on);
	void onCompToggled(bool on);
	void onLimiterToggled(bool on);
	void onMonitorToggled(bool on);
	void onMonitorGainChanged(int value);
	void onMonitorDeviceChanged();

	// Routing card (mic -> output device).
	void onRouteToggle();
	void onRouteDeviceChanged();
	void onRouteGainChanged(int value);
	void onRouteMuteToggled(bool muted);
	void onRouteNoiseToggled(bool on);
	void onRouteCompToggled(bool on);
	void onRouteLimiterToggled(bool on);

	void pollStatus(); // slow: device presence (2 s)
	void pollMeter();  // fast: level bar (only while running)

private:
	void buildUi();
	void applyRunningState();
	void applyRouteRunningState();
	void populateRouteDevices(); // refill input/output combos (preserve UID)
	void registerHotkey();
	void unregisterHotkey();
	char *configPath() const;

	static void hotkeyThunk(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey,
				bool pressed);

	std::unique_ptr<VirtualMicEngine> engine_;
	std::unique_ptr<AudioRouterEngine> router_;

	QLabel *statusLabel_ = nullptr;   // one-line: device + running state
	QPushButton *toggleBtn_ = nullptr;
	QSlider *gainSlider_ = nullptr;
	QLabel *gainLabel_ = nullptr;
	QCheckBox *muteCheck_ = nullptr;
	QProgressBar *meter_ = nullptr;
	QCheckBox *autoStartCheck_ = nullptr;

	// Processing chain
	QCheckBox *noiseCheck_ = nullptr;
	QCheckBox *compCheck_ = nullptr;
	QCheckBox *limiterCheck_ = nullptr;
	QCheckBox *monitorCheck_ = nullptr;
	QWidget *monitorRow_ = nullptr; // device + level rows, shown only when monitoring
	QComboBox *monitorCombo_ = nullptr;
	QSlider *monitorGainSlider_ = nullptr;
	QLabel *monitorGainLabel_ = nullptr;

	// Routing card (mic -> output device)
	QComboBox *inputCombo_ = nullptr;
	QComboBox *outputCombo_ = nullptr;
	QPushButton *routeBtn_ = nullptr;
	QSlider *routeGainSlider_ = nullptr;
	QLabel *routeGainLabel_ = nullptr;
	QCheckBox *routeMuteCheck_ = nullptr;
	QProgressBar *routeMeter_ = nullptr;
	QLabel *routeStatusLabel_ = nullptr;
	QCheckBox *routeNoiseCheck_ = nullptr;
	QCheckBox *routeCompCheck_ = nullptr;
	QCheckBox *routeLimiterCheck_ = nullptr;

	QTimer *statusTimer_ = nullptr;
	QTimer *meterTimer_ = nullptr;

	bool autoStart_ = false;
	bool resumeWhenDeviceReturns_ = false; // auto-recover after device loss
	obs_hotkey_id hotkeyId_ = OBS_INVALID_HOTKEY_ID;
};
