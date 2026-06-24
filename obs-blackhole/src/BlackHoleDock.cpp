#include "BlackHoleDock.h"
#include "VirtualMicEngine.h"
#include "AudioRouterEngine.h"
#include "AudioDeviceControl.h"

#include <obs.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QSlider>
#include <QCheckBox>
#include <QProgressBar>
#include <QTimer>
#include <QFont>
#include <QMetaObject>

#include <cmath>

BlackHoleDock::BlackHoleDock(QWidget *parent) : QWidget(parent)
{
	engine_ = std::make_unique<VirtualMicEngine>();
	router_ = std::make_unique<AudioRouterEngine>();
	buildUi();

	// Slow status poll (device presence). 2 s is plenty and avoids hammering
	// CoreAudio with a full device enumeration many times per second.
	statusTimer_ = new QTimer(this);
	statusTimer_->setInterval(2000);
	connect(statusTimer_, &QTimer::timeout, this, &BlackHoleDock::pollStatus);
	statusTimer_->start();

	// Fast meter poll — only runs while the mic is active (see applyRunningState).
	meterTimer_ = new QTimer(this);
	meterTimer_->setInterval(66); // ~15 fps
	connect(meterTimer_, &QTimer::timeout, this, &BlackHoleDock::pollMeter);

	loadSettings();
	registerHotkey();
	pollStatus();
}

BlackHoleDock::~BlackHoleDock()
{
	unregisterHotkey();
	if (engine_)
		engine_->stop();
	if (router_)
		router_->stop();
}

// A global, rebindable hotkey (Settings → Hotkeys → "Toggle OBS Virtual Mic").
void BlackHoleDock::registerHotkey()
{
	hotkeyId_ = obs_hotkey_register_frontend(
		"obs-blackhole.toggle", "Toggle OBS Virtual Mic", hotkeyThunk, this);

	// Restore any saved binding.
	char *path = obs_module_get_config_path(obs_current_module(), "hotkey.json");
	obs_data_t *d = obs_data_create_from_json_file(path);
	bfree(path);
	if (d) {
		obs_data_array_t *arr = obs_data_get_array(d, "bindings");
		obs_hotkey_load(hotkeyId_, arr);
		obs_data_array_release(arr);
		obs_data_release(d);
	}
}

void BlackHoleDock::unregisterHotkey()
{
	if (hotkeyId_ == OBS_INVALID_HOTKEY_ID)
		return;
	// Persist the binding.
	obs_data_array_t *arr = obs_hotkey_save(hotkeyId_);
	obs_data_t *d = obs_data_create();
	obs_data_set_array(d, "bindings", arr);
	char *dir = obs_module_get_config_path(obs_current_module(), "");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_get_config_path(obs_current_module(), "hotkey.json");
	obs_data_save_json(d, path);
	bfree(path);
	obs_data_release(d);
	obs_data_array_release(arr);

	obs_hotkey_unregister(hotkeyId_);
	hotkeyId_ = OBS_INVALID_HOTKEY_ID;
}

void BlackHoleDock::hotkeyThunk(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	// Hotkey fires off the UI thread — marshal the toggle onto it.
	auto *self = static_cast<BlackHoleDock *>(data);
	QMetaObject::invokeMethod(self, "onToggleMic", Qt::QueuedConnection);
}

// Small uppercase, muted section header that reads well on light or dark themes.
static QLabel *sectionLabel(const QString &text, QWidget *parent)
{
	auto *l = new QLabel(text.toUpper(), parent);
	QFont f = l->font();
	f.setPointSizeF(f.pointSizeF() * 0.82);
	f.setBold(true);
	f.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
	l->setFont(f);
	l->setStyleSheet("color: rgba(140,140,140,0.95);");
	return l;
}

void BlackHoleDock::buildUi()
{
	// Outer layout holds a single scroll area so the dock can be any height.
	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	outer->addWidget(scroll);

	auto *content = new QWidget(scroll);
	content->setObjectName("bhContent");
	scroll->setWidget(content);

	auto *root = new QVBoxLayout(content);
	root->setContentsMargins(12, 12, 12, 12);
	root->setSpacing(10);

	// --- Status pill ------------------------------------------------------
	statusLabel_ = new QLabel(tr("checking…"), content);
	statusLabel_->setTextFormat(Qt::RichText);
	statusLabel_->setWordWrap(true);
	statusLabel_->setStyleSheet(
		"QLabel{padding:7px 10px; border-radius:7px;"
		" background:rgba(127,127,127,0.10);}");
	root->addWidget(statusLabel_);

	// --- Big toggle button ------------------------------------------------
	toggleBtn_ = new QPushButton(tr("Start Virtual Mic"), content);
	toggleBtn_->setCheckable(true);
	toggleBtn_->setCursor(Qt::PointingHandCursor);
	QFont bf = toggleBtn_->font();
	bf.setBold(true);
	bf.setPointSizeF(bf.pointSizeF() * 1.05);
	toggleBtn_->setFont(bf);
	toggleBtn_->setMinimumHeight(40);
	connect(toggleBtn_, &QPushButton::clicked, this, &BlackHoleDock::onToggleMic);
	root->addWidget(toggleBtn_);

	// --- Level meter ------------------------------------------------------
	root->addWidget(sectionLabel(tr("Output level"), content));
	meter_ = new QProgressBar(content);
	meter_->setRange(0, 100);
	meter_->setTextVisible(false);
	meter_->setFixedHeight(10);
	meter_->setStyleSheet("QProgressBar{border:none; border-radius:5px;"
			      " background:rgba(127,127,127,0.18);}"
			      "QProgressBar::chunk{border-radius:5px; background:#2ecc71;}");
	root->addWidget(meter_);

	// --- Controls card ----------------------------------------------------
	root->addSpacing(2);
	auto *card = new QFrame(content);
	card->setObjectName("bhCard");
	card->setStyleSheet(
		"#bhCard{background:rgba(127,127,127,0.07);"
		" border:1px solid rgba(127,127,127,0.18); border-radius:8px;}");
	auto *cardLayout = new QVBoxLayout(card);
	cardLayout->setContentsMargins(12, 12, 12, 12);
	cardLayout->setSpacing(9);

	auto *gainRow = new QHBoxLayout();
	gainRow->addWidget(new QLabel(tr("Gain"), card));
	gainSlider_ = new QSlider(Qt::Horizontal, card);
	gainSlider_->setRange(-60, 24);
	gainSlider_->setValue(0);
	connect(gainSlider_, &QSlider::valueChanged, this, &BlackHoleDock::onGainChanged);
	gainRow->addWidget(gainSlider_, 1);
	gainLabel_ = new QLabel(tr("0 dB"), card);
	gainLabel_->setMinimumWidth(46);
	gainLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	gainRow->addWidget(gainLabel_);
	cardLayout->addLayout(gainRow);

	muteCheck_ = new QCheckBox(tr("Mute"), card);
	connect(muteCheck_, &QCheckBox::toggled, this, &BlackHoleDock::onMuteToggled);
	cardLayout->addWidget(muteCheck_);

	autoStartCheck_ = new QCheckBox(tr("Auto-start on launch"), card);
	connect(autoStartCheck_, &QCheckBox::toggled, this, [this](bool) { saveSettings(); });
	cardLayout->addWidget(autoStartCheck_);

	root->addWidget(card);

	// --- Processing card --------------------------------------------------
	root->addSpacing(2);
	root->addWidget(sectionLabel(tr("Processing"), content));
	auto *fx = new QFrame(content);
	fx->setObjectName("bhCard");
	fx->setStyleSheet(
		"#bhCard{background:rgba(127,127,127,0.07);"
		" border:1px solid rgba(127,127,127,0.18); border-radius:8px;}");
	auto *fxLayout = new QVBoxLayout(fx);
	fxLayout->setContentsMargins(12, 12, 12, 12);
	fxLayout->setSpacing(9);

	noiseCheck_ = new QCheckBox(tr("Noise suppression"), fx);
	noiseCheck_->setToolTip(tr("Remove background noise (RNNoise). Needs OBS at 48 kHz."));
	connect(noiseCheck_, &QCheckBox::toggled, this, &BlackHoleDock::onNoiseToggled);
	fxLayout->addWidget(noiseCheck_);

	compCheck_ = new QCheckBox(tr("Compressor"), fx);
	compCheck_->setToolTip(tr("Even out loud and quiet so your level stays consistent."));
	connect(compCheck_, &QCheckBox::toggled, this, &BlackHoleDock::onCompToggled);
	fxLayout->addWidget(compCheck_);

	limiterCheck_ = new QCheckBox(tr("Limiter (anti-clip)"), fx);
	limiterCheck_->setToolTip(tr("Stop loud peaks from clipping/distorting (ceiling ~-1 dBFS)."));
	connect(limiterCheck_, &QCheckBox::toggled, this, &BlackHoleDock::onLimiterToggled);
	fxLayout->addWidget(limiterCheck_);

	monitorCheck_ = new QCheckBox(tr("Monitor on this Mac"), fx);
	monitorCheck_->setToolTip(
		tr("Hear what the virtual mic is sending, through your speakers/headphones."));
	connect(monitorCheck_, &QCheckBox::toggled, this, &BlackHoleDock::onMonitorToggled);
	fxLayout->addWidget(monitorCheck_);

	// Monitor device + level — only visible while monitoring is on.
	monitorRow_ = new QWidget(fx);
	auto *monBox = new QVBoxLayout(monitorRow_);
	monBox->setContentsMargins(18, 0, 0, 0);
	monBox->setSpacing(6);

	auto *monDevRow = new QHBoxLayout();
	monDevRow->addWidget(new QLabel(tr("Hear on"), monitorRow_));
	monitorCombo_ = new QComboBox(monitorRow_);
	monitorCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	monitorCombo_->setMinimumContentsLength(10);
	connect(monitorCombo_, SIGNAL(currentIndexChanged(int)), this,
		SLOT(onMonitorDeviceChanged()));
	monDevRow->addWidget(monitorCombo_, 1);
	monBox->addLayout(monDevRow);

	auto *monRow = new QHBoxLayout();
	monRow->addWidget(new QLabel(tr("Level"), monitorRow_));
	monitorGainSlider_ = new QSlider(Qt::Horizontal, monitorRow_);
	monitorGainSlider_->setRange(-40, 12);
	monitorGainSlider_->setValue(0);
	connect(monitorGainSlider_, &QSlider::valueChanged, this,
		&BlackHoleDock::onMonitorGainChanged);
	monRow->addWidget(monitorGainSlider_, 1);
	monitorGainLabel_ = new QLabel(tr("0 dB"), monitorRow_);
	monitorGainLabel_->setMinimumWidth(46);
	monitorGainLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	monRow->addWidget(monitorGainLabel_);
	monBox->addLayout(monRow);

	monitorRow_->setVisible(false);
	fxLayout->addWidget(monitorRow_);

	root->addWidget(fx);

	// --- Routing card (mic -> output device), VoiceMeeter-style ----------
	root->addSpacing(2);
	root->addWidget(sectionLabel(tr("Routing (mic → device)"), content));
	auto *rt = new QFrame(content);
	rt->setObjectName("bhCard");
	rt->setStyleSheet(
		"#bhCard{background:rgba(127,127,127,0.07);"
		" border:1px solid rgba(127,127,127,0.18); border-radius:8px;}");
	auto *rtLayout = new QVBoxLayout(rt);
	rtLayout->setContentsMargins(12, 12, 12, 12);
	rtLayout->setSpacing(9);

	auto *inRow = new QHBoxLayout();
	inRow->addWidget(new QLabel(tr("Mic in"), rt));
	inputCombo_ = new QComboBox(rt);
	inputCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	inputCombo_->setMinimumContentsLength(10);
	connect(inputCombo_, SIGNAL(currentIndexChanged(int)), this,
		SLOT(onRouteDeviceChanged()));
	inRow->addWidget(inputCombo_, 1);
	rtLayout->addLayout(inRow);

	auto *outRow = new QHBoxLayout();
	outRow->addWidget(new QLabel(tr("Send to"), rt));
	outputCombo_ = new QComboBox(rt);
	outputCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	outputCombo_->setMinimumContentsLength(10);
	connect(outputCombo_, SIGNAL(currentIndexChanged(int)), this,
		SLOT(onRouteDeviceChanged()));
	outRow->addWidget(outputCombo_, 1);
	rtLayout->addLayout(outRow);

	routeBtn_ = new QPushButton(tr("Start Routing"), rt);
	routeBtn_->setCheckable(true);
	routeBtn_->setCursor(Qt::PointingHandCursor);
	routeBtn_->setMinimumHeight(36);
	connect(routeBtn_, &QPushButton::clicked, this, &BlackHoleDock::onRouteToggle);
	rtLayout->addWidget(routeBtn_);

	routeStatusLabel_ = new QLabel(QString(), rt);
	routeStatusLabel_->setTextFormat(Qt::RichText);
	routeStatusLabel_->setWordWrap(true);
	routeStatusLabel_->setStyleSheet("color: rgba(140,140,140,0.95);");
	routeStatusLabel_->setVisible(false);
	rtLayout->addWidget(routeStatusLabel_);

	routeMeter_ = new QProgressBar(rt);
	routeMeter_->setRange(0, 100);
	routeMeter_->setTextVisible(false);
	routeMeter_->setFixedHeight(8);
	routeMeter_->setStyleSheet("QProgressBar{border:none; border-radius:4px;"
				   " background:rgba(127,127,127,0.18);}"
				   "QProgressBar::chunk{border-radius:4px; background:#2ecc71;}");
	rtLayout->addWidget(routeMeter_);

	auto *rGainRow = new QHBoxLayout();
	rGainRow->addWidget(new QLabel(tr("Gain"), rt));
	routeGainSlider_ = new QSlider(Qt::Horizontal, rt);
	routeGainSlider_->setRange(-60, 24);
	routeGainSlider_->setValue(0);
	connect(routeGainSlider_, &QSlider::valueChanged, this,
		&BlackHoleDock::onRouteGainChanged);
	rGainRow->addWidget(routeGainSlider_, 1);
	routeGainLabel_ = new QLabel(tr("0 dB"), rt);
	routeGainLabel_->setMinimumWidth(46);
	routeGainLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	rGainRow->addWidget(routeGainLabel_);
	rtLayout->addLayout(rGainRow);

	routeMuteCheck_ = new QCheckBox(tr("Mute"), rt);
	connect(routeMuteCheck_, &QCheckBox::toggled, this, &BlackHoleDock::onRouteMuteToggled);
	rtLayout->addWidget(routeMuteCheck_);

	routeNoiseCheck_ = new QCheckBox(tr("Noise suppression"), rt);
	connect(routeNoiseCheck_, &QCheckBox::toggled, this, &BlackHoleDock::onRouteNoiseToggled);
	rtLayout->addWidget(routeNoiseCheck_);

	routeCompCheck_ = new QCheckBox(tr("Compressor"), rt);
	connect(routeCompCheck_, &QCheckBox::toggled, this, &BlackHoleDock::onRouteCompToggled);
	rtLayout->addWidget(routeCompCheck_);

	routeLimiterCheck_ = new QCheckBox(tr("Limiter (anti-clip)"), rt);
	connect(routeLimiterCheck_, &QCheckBox::toggled, this, &BlackHoleDock::onRouteLimiterToggled);
	rtLayout->addWidget(routeLimiterCheck_);

	root->addWidget(rt);
	root->addStretch(1);

	populateRouteDevices();

	setMinimumWidth(220);
}

// ---------------------------------------------------------------------------
void BlackHoleDock::onToggleMic()
{
	if (engine_->running()) {
		engine_->stop();
	} else {
		engine_->setGainDb(gainSlider_->value());
		engine_->setMuted(muteCheck_->isChecked());
		engine_->setNoiseSuppress(noiseCheck_->isChecked());
		engine_->setCompressor(compCheck_->isChecked());
		engine_->setLimiter(limiterCheck_->isChecked());
		engine_->setMonitorGainDb(monitorGainSlider_->value());
		engine_->setMonitorDevice(monitorCombo_->currentData().toString().toStdString());
		if (engine_->start(0 /* main mix */)) {
			// Monitoring needs the engine running, so apply it now.
			if (monitorCheck_->isChecked() &&
			    !engine_->setMonitor(true)) {
				const QSignalBlocker b(monitorCheck_);
				monitorCheck_->setChecked(false);
				monitorRow_->setVisible(false);
			}
		} else {
			toggleBtn_->setChecked(false);
		}
	}
	saveSettings();
	applyRunningState();
	pollStatus();
}

void BlackHoleDock::onGainChanged(int value)
{
	gainLabel_->setText(tr("%1 dB").arg(value));
	engine_->setGainDb(value);
	saveSettings();
}

void BlackHoleDock::onMuteToggled(bool muted)
{
	engine_->setMuted(muted);
	saveSettings();
}

void BlackHoleDock::onNoiseToggled(bool on)
{
	engine_->setNoiseSuppress(on);
	saveSettings();
}

void BlackHoleDock::onCompToggled(bool on)
{
	engine_->setCompressor(on);
	saveSettings();
}

void BlackHoleDock::onLimiterToggled(bool on)
{
	engine_->setLimiter(on);
	saveSettings();
}

void BlackHoleDock::onMonitorToggled(bool on)
{
	monitorRow_->setVisible(on);
	// setMonitor only takes effect while running; it returns false if it
	// would feed back (target == the virtual device) or the target is gone.
	engine_->setMonitorDevice(monitorCombo_->currentData().toString().toStdString());
	bool ok = engine_->setMonitor(on);
	if (on && !ok) {
		const QSignalBlocker b(monitorCheck_);
		monitorCheck_->setChecked(false);
		monitorRow_->setVisible(false);
	}
	saveSettings();
	pollStatus();
}

void BlackHoleDock::onMonitorGainChanged(int value)
{
	monitorGainLabel_->setText(tr("%1 dB").arg(value));
	engine_->setMonitorGainDb(value);
	saveSettings();
}

void BlackHoleDock::onMonitorDeviceChanged()
{
	engine_->setMonitorDevice(monitorCombo_->currentData().toString().toStdString());
	// Re-point a live monitor at the newly chosen device.
	if (monitorCheck_->isChecked() && engine_->running()) {
		engine_->setMonitor(false);
		if (!engine_->setMonitor(true)) {
			const QSignalBlocker b(monitorCheck_);
			monitorCheck_->setChecked(false);
			monitorRow_->setVisible(false);
		}
	}
	saveSettings();
}

// ---- Routing (mic -> output device) ---------------------------------------

// Fill the input/output combos from the live device list, preserving the
// current UID selection (devices carry their UID in itemData).
void BlackHoleDock::populateRouteDevices()
{
	const QString prevIn = inputCombo_->currentData().toString();
	const QString prevOut = outputCombo_->currentData().toString();
	const QString prevMon = monitorCombo_ ? monitorCombo_->currentData().toString() : QString();

	const QSignalBlocker bi(inputCombo_);
	const QSignalBlocker bo(outputCombo_);
	inputCombo_->clear();
	outputCombo_->clear();
	if (monitorCombo_) {
		const QSignalBlocker bm(monitorCombo_);
		monitorCombo_->clear();
		// Empty UID = follow the system default output device.
		monitorCombo_->addItem(tr("System default output"), QString());
	}

	for (const auto &d : AudioDeviceControl::listDevices()) {
		QString name = QString::fromStdString(d.name);
		QString uid = QString::fromStdString(d.uid);
		if (d.inputChannels > 0)
			inputCombo_->addItem(name, uid);
		if (d.outputChannels > 0) {
			outputCombo_->addItem(name, uid);
			if (monitorCombo_) {
				const QSignalBlocker bm(monitorCombo_);
				monitorCombo_->addItem(name, uid);
			}
		}
	}

	int ii = inputCombo_->findData(prevIn);
	if (ii >= 0)
		inputCombo_->setCurrentIndex(ii);
	int oi = outputCombo_->findData(prevOut);
	if (oi >= 0)
		outputCombo_->setCurrentIndex(oi);
	if (monitorCombo_) {
		const QSignalBlocker bm(monitorCombo_);
		int mi = monitorCombo_->findData(prevMon);
		monitorCombo_->setCurrentIndex(mi >= 0 ? mi : 0);
	}
}

void BlackHoleDock::onRouteToggle()
{
	if (router_->running()) {
		router_->stop();
	} else {
		router_->setGainDb(routeGainSlider_->value());
		router_->setMuted(routeMuteCheck_->isChecked());
		router_->setNoiseSuppress(routeNoiseCheck_->isChecked());
		router_->setCompressor(routeCompCheck_->isChecked());
		router_->setLimiter(routeLimiterCheck_->isChecked());
		std::string inUid = inputCombo_->currentData().toString().toStdString();
		std::string outUid = outputCombo_->currentData().toString().toStdString();
		if (!router_->start(inUid, outUid)) {
			routeBtn_->setChecked(false);
			routeStatusLabel_->setText(
				tr("<span style='color:#e67e22;'>Couldn't start — pick a "
				   "different mic and output (they can't be the same device).</span>"));
			routeStatusLabel_->setVisible(true);
		}
	}
	saveSettings();
	applyRouteRunningState();
}

void BlackHoleDock::onRouteDeviceChanged()
{
	// Switching devices restarts the route so the change takes effect.
	if (router_->running()) {
		router_->stop();
		std::string inUid = inputCombo_->currentData().toString().toStdString();
		std::string outUid = outputCombo_->currentData().toString().toStdString();
		if (!router_->start(inUid, outUid))
			routeBtn_->setChecked(false);
	}
	saveSettings();
	applyRouteRunningState();
}

void BlackHoleDock::onRouteGainChanged(int value)
{
	routeGainLabel_->setText(tr("%1 dB").arg(value));
	router_->setGainDb(value);
	saveSettings();
}

void BlackHoleDock::onRouteMuteToggled(bool muted)
{
	router_->setMuted(muted);
	saveSettings();
}

void BlackHoleDock::onRouteNoiseToggled(bool on)
{
	router_->setNoiseSuppress(on);
	saveSettings();
}

void BlackHoleDock::onRouteCompToggled(bool on)
{
	router_->setCompressor(on);
	saveSettings();
}

void BlackHoleDock::onRouteLimiterToggled(bool on)
{
	router_->setLimiter(on);
	saveSettings();
}

void BlackHoleDock::applyRouteRunningState()
{
	const bool running = router_->running();
	routeBtn_->setChecked(running);
	routeBtn_->setText(running ? tr("Stop Routing") : tr("Start Routing"));

	const QString base = running ? "#e74c3c" : "#27ae60";
	const QString hover = running ? "#ec5e4f" : "#2ecc71";
	const QString down = running ? "#c0392b" : "#1e8e4f";
	routeBtn_->setStyleSheet(QString("QPushButton{background:%1; color:white;"
					 " border:none; border-radius:7px; padding:8px;}"
					 "QPushButton:hover{background:%2;}"
					 "QPushButton:pressed{background:%3;}")
					 .arg(base, hover, down));

	if (running) {
		routeStatusLabel_->setText(
			tr("<span style='color:#2ecc71;'>● Live</span> · %1 → %2")
				.arg(QString::fromStdString(router_->inputName()),
				     QString::fromStdString(router_->outputName())));
		routeStatusLabel_->setVisible(true);
		if (!meterTimer_->isActive())
			meterTimer_->start();
	} else {
		routeStatusLabel_->setVisible(false);
		routeMeter_->setValue(0);
		if (!engine_->running())
			meterTimer_->stop();
	}
}

// ---------------------------------------------------------------------------
void BlackHoleDock::applyRunningState()
{
	const bool running = engine_->running();
	toggleBtn_->setChecked(running);
	toggleBtn_->setText(running ? tr("Stop Virtual Mic") : tr("Start Virtual Mic"));

	// Accent the button: inviting green to start, clear red to stop.
	const QString base = running ? "#e74c3c" : "#27ae60";
	const QString hover = running ? "#ec5e4f" : "#2ecc71";
	const QString down = running ? "#c0392b" : "#1e8e4f";
	toggleBtn_->setStyleSheet(QString("QPushButton{background:%1; color:white;"
					  " border:none; border-radius:7px; padding:9px;}"
					  "QPushButton:hover{background:%2;}"
					  "QPushButton:pressed{background:%3;}")
					  .arg(base, hover, down));

	if (running) {
		if (!meterTimer_->isActive())
			meterTimer_->start();
	} else {
		meter_->setValue(0);
		// Keep the meter timer alive if the router is still running.
		if (!router_ || !router_->running())
			meterTimer_->stop();
	}
}

// Convert a 0..1 peak to a 0..100 meter percentage (dBFS over a 60 dB floor).
static int peakToPct(float peak)
{
	if (peak <= 0.0001f)
		return 0;
	float db = 20.0f * std::log10(peak);
	int pct = static_cast<int>((db + 60.0f) / 60.0f * 100.0f);
	return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

// Update one meter bar; re-applies the chunk colour only when it changes.
static void updateMeter(QProgressBar *bar, int pct, int radius, const char *&lastColour)
{
	const char *colour = pct >= 95 ? "#e74c3c" : (pct >= 80 ? "#f1c40f" : "#2ecc71");
	if (colour != lastColour) {
		lastColour = colour;
		bar->setStyleSheet(
			QString("QProgressBar{border:none; border-radius:%1px;"
				" background:rgba(127,127,127,0.18);}"
				"QProgressBar::chunk{border-radius:%1px; background:%2;}")
				.arg(radius)
				.arg(colour));
	}
	bar->setValue(pct);
}

void BlackHoleDock::pollMeter()
{
	static const char *micColour = nullptr;
	float peak = engine_->running() ? engine_->peakLevel() : 0.0f;
	updateMeter(meter_, peakToPct(peak), 5, micColour);

	static const char *routeColour = nullptr;
	float rpeak = router_->running() ? router_->peakLevel() : 0.0f;
	updateMeter(routeMeter_, peakToPct(rpeak), 4, routeColour);
}

void BlackHoleDock::pollStatus()
{
	std::string name;
	AudioDeviceID dev = AudioDeviceControl::findVirtualDevice(&name);
	const bool live = (dev != kAudioObjectUnknown);
	bool running = engine_->running();

	// Auto-recover: if the device vanished mid-run (e.g. coreaudiod reload),
	// stop cleanly and remember to resume once it comes back.
	if (running && !live) {
		engine_->stop();
		resumeWhenDeviceReturns_ = true;
		running = false;
	} else if (!running && live && resumeWhenDeviceReturns_) {
		if (engine_->start(0 /* main mix */)) {
			resumeWhenDeviceReturns_ = false;
			running = true;
		}
	}

	auto dot = [](const QString &colour) {
		return QString("<span style='color:%1;font-size:15px;'>●</span>&nbsp;").arg(colour);
	};
	QString text;
	if (!live) {
		bool onDisk = AudioDeviceControl::driverInstalledOnDisk();
		text = dot("#e67e22") +
		       (onDisk ? tr("Device installed — reload coreaudiod")
			       : tr("No “OBS Audio” device found"));
	} else if (running) {
		text = dot("#2ecc71") + tr("Live") +
		       QString(" · <b>%1</b>").arg(QString::fromStdString(name));
	} else {
		text = dot("#7f8c8d") + tr("Ready") +
		       QString(" · <b>%1</b>").arg(QString::fromStdString(name));
	}
	statusLabel_->setText(text);

	// Noise suppression (RNNoise) only works at 48 kHz — grey it out and
	// explain when OBS is configured for a different rate.
	obs_audio_info oai{};
	bool ns48k = obs_get_audio_info(&oai) && oai.samples_per_sec == 48000;
	if (noiseCheck_->isEnabled() != ns48k) {
		noiseCheck_->setEnabled(ns48k);
		noiseCheck_->setToolTip(
			ns48k ? tr("Remove background noise (RNNoise).")
			      : tr("Needs OBS audio at 48 kHz "
				   "(Settings → Audio → Sample Rate)."));
	}

	// Refresh the routing device dropdowns when devices are plugged/unplugged.
	// Cheap signature = count + concatenated UIDs; repopulate only on change.
	{
		auto devs = AudioDeviceControl::listDevices();
		std::string sig;
		for (auto &d : devs)
			sig += d.uid + "|";
		static std::string lastSig;
		if (sig != lastSig) {
			lastSig = sig;
			populateRouteDevices();
		}
	}

	applyRunningState();
	applyRouteRunningState();
}

// ---------------------------------------------------------------------------
char *BlackHoleDock::configPath() const
{
	return obs_module_get_config_path(obs_current_module(), "settings.json");
}

void BlackHoleDock::loadSettings()
{
	char *path = configPath();
	obs_data_t *d = obs_data_create_from_json_file(path);
	bfree(path);
	if (!d)
		return;

	obs_data_set_default_double(d, "gain_db", 0.0);
	obs_data_set_default_bool(d, "muted", false);
	obs_data_set_default_bool(d, "auto_start", false);
	obs_data_set_default_bool(d, "noise_suppress", false);
	obs_data_set_default_bool(d, "compressor", false);
	obs_data_set_default_bool(d, "limiter", false);
	obs_data_set_default_bool(d, "monitor", false);
	obs_data_set_default_double(d, "monitor_db", 0.0);
	obs_data_set_default_string(d, "monitor_uid", "");
	obs_data_set_default_string(d, "route_input_uid", "");
	obs_data_set_default_string(d, "route_output_uid", "");
	obs_data_set_default_double(d, "route_gain_db", 0.0);
	obs_data_set_default_bool(d, "route_muted", false);
	obs_data_set_default_bool(d, "route_ns", false);
	obs_data_set_default_bool(d, "route_comp", false);
	obs_data_set_default_bool(d, "route_limiter", false);

	const QSignalBlocker b2(gainSlider_);
	const QSignalBlocker b3(muteCheck_);
	const QSignalBlocker b4(autoStartCheck_);
	const QSignalBlocker b5(noiseCheck_);
	const QSignalBlocker b6(compCheck_);
	const QSignalBlocker b7(limiterCheck_);
	const QSignalBlocker b8(monitorCheck_);
	const QSignalBlocker b9(monitorGainSlider_);
	const QSignalBlocker b10(monitorCombo_);
	const QSignalBlocker br1(inputCombo_);
	const QSignalBlocker br2(outputCombo_);
	const QSignalBlocker br3(routeGainSlider_);
	const QSignalBlocker br4(routeMuteCheck_);
	const QSignalBlocker br5(routeNoiseCheck_);
	const QSignalBlocker br6(routeCompCheck_);
	const QSignalBlocker br7(routeLimiterCheck_);

	int gain = (int)obs_data_get_double(d, "gain_db");
	gainSlider_->setValue(gain);
	gainLabel_->setText(tr("%1 dB").arg(gain));
	muteCheck_->setChecked(obs_data_get_bool(d, "muted"));
	autoStart_ = obs_data_get_bool(d, "auto_start");
	autoStartCheck_->setChecked(autoStart_);

	noiseCheck_->setChecked(obs_data_get_bool(d, "noise_suppress"));
	compCheck_->setChecked(obs_data_get_bool(d, "compressor"));
	limiterCheck_->setChecked(obs_data_get_bool(d, "limiter"));
	monitorCheck_->setChecked(obs_data_get_bool(d, "monitor"));
	int mdb = (int)obs_data_get_double(d, "monitor_db");
	monitorGainSlider_->setValue(mdb);
	monitorGainLabel_->setText(tr("%1 dB").arg(mdb));
	int monIdx = monitorCombo_->findData(QString::fromUtf8(obs_data_get_string(d, "monitor_uid")));
	monitorCombo_->setCurrentIndex(monIdx >= 0 ? monIdx : 0);
	monitorRow_->setVisible(monitorCheck_->isChecked());

	engine_->setGainDb(gain);
	engine_->setMuted(muteCheck_->isChecked());
	engine_->setNoiseSuppress(noiseCheck_->isChecked());
	engine_->setCompressor(compCheck_->isChecked());
	engine_->setLimiter(limiterCheck_->isChecked());
	engine_->setMonitorGainDb(mdb);
	engine_->setMonitorDevice(monitorCombo_->currentData().toString().toStdString());

	// Routing card
	int ii = inputCombo_->findData(QString::fromUtf8(obs_data_get_string(d, "route_input_uid")));
	if (ii >= 0)
		inputCombo_->setCurrentIndex(ii);
	int oi = outputCombo_->findData(QString::fromUtf8(obs_data_get_string(d, "route_output_uid")));
	if (oi >= 0)
		outputCombo_->setCurrentIndex(oi);
	int rgain = (int)obs_data_get_double(d, "route_gain_db");
	routeGainSlider_->setValue(rgain);
	routeGainLabel_->setText(tr("%1 dB").arg(rgain));
	routeMuteCheck_->setChecked(obs_data_get_bool(d, "route_muted"));
	routeNoiseCheck_->setChecked(obs_data_get_bool(d, "route_ns"));
	routeCompCheck_->setChecked(obs_data_get_bool(d, "route_comp"));
	routeLimiterCheck_->setChecked(obs_data_get_bool(d, "route_limiter"));

	router_->setGainDb(rgain);
	router_->setMuted(routeMuteCheck_->isChecked());
	router_->setNoiseSuppress(routeNoiseCheck_->isChecked());
	router_->setCompressor(routeCompCheck_->isChecked());
	router_->setLimiter(routeLimiterCheck_->isChecked());

	obs_data_release(d);
}

void BlackHoleDock::saveSettings()
{
	obs_data_t *d = obs_data_create();
	obs_data_set_double(d, "gain_db", gainSlider_->value());
	obs_data_set_bool(d, "muted", muteCheck_->isChecked());
	obs_data_set_bool(d, "auto_start", autoStartCheck_->isChecked());
	obs_data_set_bool(d, "noise_suppress", noiseCheck_->isChecked());
	obs_data_set_bool(d, "compressor", compCheck_->isChecked());
	obs_data_set_bool(d, "limiter", limiterCheck_->isChecked());
	obs_data_set_bool(d, "monitor", monitorCheck_->isChecked());
	obs_data_set_double(d, "monitor_db", monitorGainSlider_->value());
	obs_data_set_string(d, "monitor_uid",
			    monitorCombo_->currentData().toString().toUtf8().constData());
	obs_data_set_string(d, "route_input_uid",
			    inputCombo_->currentData().toString().toUtf8().constData());
	obs_data_set_string(d, "route_output_uid",
			    outputCombo_->currentData().toString().toUtf8().constData());
	obs_data_set_double(d, "route_gain_db", routeGainSlider_->value());
	obs_data_set_bool(d, "route_muted", routeMuteCheck_->isChecked());
	obs_data_set_bool(d, "route_ns", routeNoiseCheck_->isChecked());
	obs_data_set_bool(d, "route_comp", routeCompCheck_->isChecked());
	obs_data_set_bool(d, "route_limiter", routeLimiterCheck_->isChecked());

	char *dir = obs_module_get_config_path(obs_current_module(), "");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = configPath();
	obs_data_save_json(d, path);
	bfree(path);
	obs_data_release(d);
}

void BlackHoleDock::maybeAutoStart()
{
	if (!autoStart_)
		return;
	if (engine_->start(0 /* main mix */))
		applyRunningState();
}
