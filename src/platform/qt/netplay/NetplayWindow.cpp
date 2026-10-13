/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "NetplayWindow.h"

#include "ConfigController.h"
#include "InputController.h"
#include "LogController.h"
#include "NetplayAudio.h"
#include "NetplayView.h"

#include <MatchSession.h>
#include <PeerLinks.h>

#include <mgba/feature/netplay.h>

#include <QCheckBox>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTextEdit>
#include <QVBoxLayout>

#ifdef BUILD_QT_MULTIMEDIA
#include <QAudioFormat>
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#include <QAudioSink>
#include <QMediaDevices>
#else
#include <QAudioOutput>
#endif
#endif

#include <random>

using namespace QGBA;
using namespace NetplayOnline;

namespace {

const char* const GROUP = "netplay";

const char* const STYLE =
	"QGBA--NetplayWindow, QWidget#setup, QWidget#match { background: #1b1726; color: #e8e4f0; }"
	"QLabel { color: #e8e4f0; }"
	"QLabel#title { color: #2ee6d6; font-weight: bold; font-size: 15px; }"
	"QLabel#heading { color: #2ee6d6; font-weight: bold; }"
	"QLabel#status { color: #b8b0cc; }"
	"QWidget#side { background: #241d33; }"
	"QWidget#bar { background: #15111f; }"
	"QLineEdit, QSpinBox, QTextEdit { background: #15111f; color: #e8e4f0; border: 1px solid #3a3150; padding: 3px; }"
	"QPushButton { background: #3a3150; color: #e8e4f0; border: 1px solid #4c4266; padding: 5px 12px; }"
	"QPushButton:hover { background: #4c4266; }"
	"QPushButton#primary { background: #1f7f78; border-color: #2ee6d6; }"
	"QCheckBox { color: #e8e4f0; }";

const char* const PLAYER_COLORS[] = { "#ff4f9a", "#2ee6d6", "#ffc94d", "#8f7bff" };

QString escape(const QString& text) {
	return text.toHtmlEscaped();
}

}

NetplayWindow::NetplayWindow(ConfigController* config, InputController* input, LogController* log, QWidget* parent)
	: QWidget(parent, Qt::Window)
	, m_config(config)
	, m_input(input) {
	m_logger.controller = log;
	m_logger.filter = log ? log->filter() : nullptr;
	m_logger.log = [](mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
		LogController* controller = static_cast<RunnerLogger*>(logger)->controller;
		if (controller) {
			QString message = QString::vasprintf(format, args);
			QMetaObject::invokeMethod(controller, "postLog", Qt::QueuedConnection, Q_ARG(int, level), Q_ARG(int, category), Q_ARG(const QString&, message));
		}
	};
	setWindowTitle(tr("Netplay"));
	setAttribute(Qt::WA_DeleteOnClose);
	setStyleSheet(QString::fromLatin1(STYLE));
	m_pages = new QStackedWidget(this);
	m_pages->addWidget(buildSetupPage());
	m_pages->addWidget(buildMatchPage());
	QVBoxLayout* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(m_pages);
	resize(960, 600);
	m_refresh.setInterval(16);
	connect(&m_refresh, &QTimer::timeout, this, &NetplayWindow::refresh);
}

NetplayWindow::~NetplayWindow() {
	m_refresh.stop();
	if (m_runner) {
		m_runner->requestStop();
		m_runner->wait();
	}
	delete m_audioOutput;
}

QWidget* NetplayWindow::buildSetupPage() {
	QWidget* page = new QWidget;
	page->setObjectName("setup");
	QVBoxLayout* layout = new QVBoxLayout(page);
	layout->setContentsMargins(24, 20, 24, 20);
	QLabel* title = new QLabel(tr("NETPLAY"));
	title->setObjectName("title");
	layout->addWidget(title);
	QLabel* intro = new QLabel(tr("Rollback netplay for linked GBAs. A profile names the ROMs, the link and where each "
	                              "player's keys go (see doc/netplay.md)."));
	intro->setWordWrap(true);
	intro->setObjectName("status");
	layout->addWidget(intro);

	QFormLayout* form = new QFormLayout;
	m_profile = new QLineEdit(m_config->getQtOption("profile", GROUP).toString());
	QPushButton* browse = new QPushButton(tr("Browse..."));
	connect(browse, &QPushButton::clicked, this, &NetplayWindow::browseProfile);
	QHBoxLayout* profileRow = new QHBoxLayout;
	profileRow->addWidget(m_profile);
	profileRow->addWidget(browse);
	form->addRow(tr("Profile"), profileRow);
	m_name = new QLineEdit(m_config->getQtOption("name", GROUP).toString());
	m_name->setMaxLength(16);
	m_name->setPlaceholderText(tr("2 to 16 characters"));
	form->addRow(tr("Name"), m_name);
	QVariant host = m_config->getQtOption("host", GROUP);
	m_host = new QLineEdit(host.isNull() ? QStringLiteral("127.0.0.1") : host.toString());
	m_port = new QSpinBox;
	m_port->setRange(1, 65535);
	QVariant port = m_config->getQtOption("port", GROUP);
	m_port->setValue(port.isNull() ? 7350 : port.toInt());
	m_ssl = new QCheckBox(tr("TLS"));
	m_ssl->setChecked(m_config->getQtOption("ssl", GROUP).toBool());
	QHBoxLayout* serverRow = new QHBoxLayout;
	serverRow->addWidget(m_host, 1);
	serverRow->addWidget(m_port);
	serverRow->addWidget(m_ssl);
	form->addRow(tr("Nakama server"), serverRow);
	QVariant key = m_config->getQtOption("key", GROUP);
	m_key = new QLineEdit(key.isNull() ? QStringLiteral("defaultkey") : key.toString());
	form->addRow(tr("Server key"), m_key);
	layout->addLayout(form);

	QHBoxLayout* buttons = new QHBoxLayout;
	QPushButton* local = new QPushButton(tr("Play on this computer"));
	QPushButton* stress = new QPushButton(tr("Stress test"));
	stress->setToolTip(tr("Rolls back every frame and compares checksums, to check that the game stays deterministic"));
	QPushButton* online = new QPushButton(tr("Find a match"));
	online->setObjectName("primary");
	connect(local, &QPushButton::clicked, this, [this]() { startLocal(false); });
	connect(stress, &QPushButton::clicked, this, [this]() { startLocal(true); });
	connect(online, &QPushButton::clicked, this, &NetplayWindow::startOnline);
	m_startButtons = { local, stress, online };
	buttons->addWidget(local);
	buttons->addWidget(stress);
	buttons->addStretch();
	buttons->addWidget(online);
	layout->addLayout(buttons);
	m_setupStatus = new QLabel;
	m_setupStatus->setObjectName("status");
	m_setupStatus->setWordWrap(true);
	layout->addWidget(m_setupStatus);
	layout->addStretch();
	return page;
}

QWidget* NetplayWindow::buildMatchPage() {
	QWidget* page = new QWidget;
	page->setObjectName("match");
	QHBoxLayout* columns = new QHBoxLayout(page);
	columns->setContentsMargins(0, 0, 0, 0);
	columns->setSpacing(0);

	QWidget* main = new QWidget;
	QVBoxLayout* mainLayout = new QVBoxLayout(main);
	mainLayout->setContentsMargins(0, 0, 0, 0);
	mainLayout->setSpacing(0);
	QWidget* top = new QWidget;
	top->setObjectName("bar");
	QHBoxLayout* topLayout = new QHBoxLayout(top);
	topLayout->setContentsMargins(14, 6, 14, 6);
	m_title = new QLabel;
	m_title->setObjectName("title");
	topLayout->addWidget(m_title, 1);
	m_leave = new QPushButton(tr("Leave"));
	connect(m_leave, &QPushButton::clicked, this, [this]() { endMatch(tr("You left the match.")); });
	topLayout->addWidget(m_leave);
	mainLayout->addWidget(top);
	m_view = new NetplayView;
	m_view->setFocusPolicy(Qt::StrongFocus);
	mainLayout->addWidget(m_view, 1);
	QWidget* bottom = new QWidget;
	bottom->setObjectName("bar");
	QHBoxLayout* bottomLayout = new QHBoxLayout(bottom);
	bottomLayout->setContentsMargins(14, 6, 14, 6);
	// both labels may be cut short, so that the window can be narrower than their text
	m_connection = new QLabel;
	m_connection->setObjectName("status");
	m_connection->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	bottomLayout->addWidget(m_connection, 3);
	m_keyHints = new QLabel;
	m_keyHints->setObjectName("status");
	m_keyHints->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	m_keyHints->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	bottomLayout->addWidget(m_keyHints, 2);
	mainLayout->addWidget(bottom);
	columns->addWidget(main, 1);

	m_side = new QWidget;
	m_side->setObjectName("side");
	m_side->setFixedWidth(320);
	QVBoxLayout* side = new QVBoxLayout(m_side);
	side->setContentsMargins(12, 10, 12, 10);
	QLabel* playersHeading = new QLabel(tr("PLAYERS"));
	playersHeading->setObjectName("heading");
	side->addWidget(playersHeading);
	m_players = new QLabel;
	m_players->setTextFormat(Qt::RichText);
	m_players->setWordWrap(true);
	side->addWidget(m_players);
	m_screens = new QWidget;
	m_screensLayout = new QVBoxLayout(m_screens);
	m_screensLayout->setContentsMargins(0, 0, 0, 0);
	side->addWidget(m_screens);
	QLabel* chatHeading = new QLabel(tr("CHAT"));
	chatHeading->setObjectName("heading");
	side->addWidget(chatHeading);
	m_chatLog = new QTextEdit;
	m_chatLog->setReadOnly(true);
	m_chatLog->setFocusPolicy(Qt::NoFocus);
	side->addWidget(m_chatLog, 1);
	m_chatInput = new QLineEdit;
	m_chatInput->setMaxLength(200);
	m_chatInput->setPlaceholderText(tr("Game keys are let go while typing"));
	m_chatInput->installEventFilter(this);
	connect(m_chatInput, &QLineEdit::returnPressed, this, &NetplayWindow::sendChat);
	side->addWidget(m_chatInput);
	columns->addWidget(m_side);
	return page;
}

void NetplayWindow::browseProfile() {
	QString path = QFileDialog::getOpenFileName(this, tr("Select a netplay profile"), m_profile->text(), tr("Netplay profiles (*.ini)"));
	if (!path.isEmpty()) {
		m_profile->setText(path);
	}
}

void NetplayWindow::saveSettings() {
	m_config->setQtOption("profile", m_profile->text(), GROUP);
	m_config->setQtOption("name", m_name->text(), GROUP);
	m_config->setQtOption("host", m_host->text(), GROUP);
	m_config->setQtOption("port", m_port->value(), GROUP);
	m_config->setQtOption("key", m_key->text(), GROUP);
	m_config->setQtOption("ssl", m_ssl->isChecked(), GROUP);
}

QString NetplayWindow::deviceId() {
	QString id = m_config->getQtOption("deviceId", GROUP).toString();
	if (id.size() < 10) {
		std::random_device device;
		QByteArray raw(16, Qt::Uninitialized);
		for (int i = 0; i < raw.size(); ++i) {
			raw[i] = char(device() & 0xFF);
		}
		id = QString::fromLatin1(raw.toHex());
		m_config->setQtOption("deviceId", id, GROUP);
	}
	return id;
}

bool NetplayWindow::loadProfileInfo(const QString& path, QString* error) {
	std::unique_ptr<mNetplayProfile> profile(new mNetplayProfile);
	char err[512];
	if (!mNetplayProfileLoad(profile.get(), path.toLocal8Bit().constData(), err, sizeof(err))) {
		*error = QString::fromLocal8Bit(err);
		return false;
	}
	char identity[41];
	if (!mNetplayProfileIdentity(profile.get(), identity, err, sizeof(err))) {
		*error = QString::fromLocal8Bit(err);
		return false;
	}
	m_profilePath = path;
	m_profileName = QString::fromUtf8(profile->name);
	m_identity = QString::fromLatin1(identity);
	m_profilePlayers = profile->players;
	m_profileAll = profile->screens == NETPLAY_SCREENS_ALL;
	m_playerNames.clear();
	for (int i = 0; i < m_profilePlayers; ++i) {
		m_playerNames.append(tr("Player %1").arg(i + 1));
	}
	return true;
}

void NetplayWindow::startLocal(bool stressTest) {
	QString error;
	if (!loadProfileInfo(m_profile->text(), &error)) {
		m_setupStatus->setText(error);
		return;
	}
	saveSettings();
	m_mode = stressTest ? Mode::StressTest : Mode::Local;
	NetplayRunner::Options options;
	options.profilePath = m_profilePath;
	options.mode = stressTest ? NetplayRunner::Mode::StressTest : NetplayRunner::Mode::Local;
	options.drawAll = m_profileAll;
	QString name = m_name->text().trimmed();
	if (!name.isEmpty()) {
		m_playerNames[0] = name;
	}
	beginMatchScreen(stressTest ? tr("stress test") : tr("on this computer"));
	startRunner(options);
}

void NetplayWindow::startOnline() {
	QString error;
	if (!loadProfileInfo(m_profile->text(), &error)) {
		m_setupStatus->setText(error);
		return;
	}
	QString name = m_name->text().trimmed();
	if (name.size() < 2) {
		m_setupStatus->setText(tr("Enter a name of 2 to 16 characters."));
		return;
	}
	saveSettings();
	m_mode = Mode::Online;
	MatchSession::QuickMatch q;
	q.server.host = m_host->text().trimmed();
	q.server.port = m_port->value();
	q.server.serverKey = m_key->text();
	q.server.ssl = m_ssl->isChecked();
	q.deviceId = deviceId();
	q.name = name;
	q.kind = QStringLiteral("mgba");
	q.identity = m_identity;
	q.players = m_profilePlayers;
	q.stunServers = m_config->getQtOption("stun", GROUP).toStringList();
	if (q.stunServers.isEmpty()) {
		q.stunServers.append(QStringLiteral("stun.l.google.com:19302"));
	}
	MatchSession* session = createSession(-1);
	beginMatchScreen(tr("online"));
	session->startQuickMatch(q);
}

void NetplayWindow::startFromLauncher(const ConnectInfo& info) {
	QString error;
	if (!loadProfileInfo(info.profile, &error)) {
		m_setupStatus->setText(error);
		show();
		return;
	}
	if (!info.identity.isEmpty() && info.identity != m_identity) {
		m_setupStatus->setText(tr("The launcher matched this game with identity %1, but this mGBA computes %2: the emulator "
		                          "build, a ROM or a save differs.").arg(info.identity.left(8), m_identity.left(8)));
		show();
		return;
	}
	m_profile->setText(info.profile);
	m_mode = Mode::Online;
	m_fromLauncher = true;
	MatchSession* session = createSession(info.inputDelay);
	beginMatchScreen(tr("online"));
	m_view->setPlaceholder(tr("Joining the match..."));
	session->startFromLauncher(info);
	show();
}

MatchSession* NetplayWindow::createSession(int inputDelay) {
	MatchSession* session = new MatchSession(this);
	m_session = session;
	connect(session, &MatchSession::stateChanged, this, [this](MatchSession::State state) {
		switch (state) {
		case MatchSession::State::SigningIn:
			m_view->setPlaceholder(tr("Signing in..."));
			break;
		case MatchSession::State::Connecting:
			m_view->setPlaceholder(tr("Connecting to the server..."));
			break;
		case MatchSession::State::Matchmaking:
			m_view->setPlaceholder(tr("Looking for %n other player(s)...", nullptr, m_profilePlayers - 1));
			break;
		case MatchSession::State::Joining:
			m_view->setPlaceholder(tr("Joining the match..."));
			break;
		case MatchSession::State::Linking:
			m_view->setPlaceholder(tr("Connecting to the other players..."));
			break;
		default:
			break;
		}
	});
	connect(session, &MatchSession::linking, this, [this, session, inputDelay]() {
		m_playerNames.clear();
		for (const MatchPlayer& p : session->players()) {
			m_playerNames.append(p.name);
		}
		NetplayRunner::Options options;
		options.profilePath = m_profilePath;
		options.mode = NetplayRunner::Mode::Online;
		options.localPlayer = session->self();
		for (const MatchPlayer& p : session->players()) {
			options.playerIds.append(p.userId);
		}
		options.links = session->links();
		options.stunServers = session->stunServers();
		options.port = session->port();
		options.inputDelay = inputDelay;
		options.drawAll = m_profileAll;
		startRunner(options);
	});
	connect(session, &MatchSession::playersChanged, this, [this, session]() {
		m_playerNames.clear();
		for (const MatchPlayer& p : session->players()) {
			m_playerNames.append(p.name);
		}
		updatePanels();
	});
	connect(session, &MatchSession::chatReceived, this, &NetplayWindow::appendChat);
	// every GBA needs every player's keys, so the match ends when anyone leaves
	connect(session, &MatchSession::playerLeft, this, [this](int player) {
		endMatch(tr("%1 left the match.").arg(m_playerNames.value(player)));
	});
	connect(session, &MatchSession::failed, this, [this](const QString& error) { endMatch(error); });
	return session;
}

void NetplayWindow::startRunner(NetplayRunner::Options options) {
	options.logger = &m_logger;
	m_runner.reset(new NetplayRunner(options));
	connect(m_runner.get(), &NetplayRunner::linked, this, [this](bool ok, const QString& error) {
		if (m_session) {
			m_session->setLinked(ok, error);
		}
		if (ok) {
			m_view->setPlaceholder(tr("Starting..."));
		}
	});
	connect(m_runner.get(), &NetplayRunner::sessionStarted, this, [this]() {
		m_started = true;
		appendChat(QString(), tr("The session started. Every computer runs every GBA; only keys travel."));
	});
	connect(m_runner.get(), &NetplayRunner::playerDisconnected, this, [this](int player) {
		endMatch(tr("%1 lost the connection.").arg(m_playerNames.value(player, tr("Player %1").arg(player + 1))));
	});
	connect(m_runner.get(), &NetplayRunner::desyncDetected, this, [this](int frame) {
		appendChat(QString(), tr("Desync detected at frame %1: the games have diverged.").arg(frame));
	});
	connect(m_runner.get(), &NetplayRunner::finished, this, [this](const QString& error) {
		endMatch(error.isEmpty() ? tr("The session ended.") : error);
	});
#ifdef BUILD_QT_MULTIMEDIA
	m_audio = new NetplayAudio(this);
	m_audio->setRunner(m_runner.get());
	QAudioFormat format;
	format.setSampleRate(48000);
	format.setChannelCount(2);
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
	format.setSampleFormat(QAudioFormat::Int16);
	QAudioSink* sink = new QAudioSink(QMediaDevices::defaultAudioOutput(), format, this);
	sink->setBufferSize(48000 * 4 / 20);
	sink->start(m_audio);
	m_audioOutput = sink;
#else
	format.setSampleSize(16);
	format.setCodec("audio/pcm");
	format.setByteOrder(QAudioFormat::LittleEndian);
	format.setSampleType(QAudioFormat::SignedInt);
	QAudioOutput* output = new QAudioOutput(format, this);
	output->setBufferSize(48000 * 4 / 20);
	output->start(m_audio);
	m_audioOutput = output;
#endif
#endif
	m_runner->start();
	m_refresh.start();
}

void NetplayWindow::beginMatchScreen(const QString& subtitle) {
	for (QPushButton* b : m_startButtons) {
		b->setEnabled(false);
	}
	m_title->setText(QStringLiteral("%1  ·  %2  ·  %3").arg(m_profileName.toUpper(), subtitle,
	                                                        tr("%n player(s)", nullptr, m_profilePlayers)));
	m_view->setPlaceholder(tr("Starting..."));
	m_chatLog->clear();
	m_chatInput->setEnabled(m_mode == Mode::Online);
	updateKeyHints();
	m_pages->setCurrentIndex(1);
	m_view->setFocus();
	updatePanels();
	updateOtherScreens();
}

void NetplayWindow::endMatch(const QString& message) {
	if (!m_endMessage.isEmpty()) {
		return;
	}
	m_endMessage = message;
	m_refresh.stop();
	delete m_audioOutput;
	m_audioOutput = nullptr;
	if (m_runner) {
		m_runner->requestStop();
		m_runner->wait();
	}
	if (m_session) {
		m_session->leave();
	}
	appendChat(QString(), message);
	m_view->setPlaceholder(message);
	disconnect(m_leave, nullptr, nullptr, nullptr);
	if (m_fromLauncher) {
		m_leave->setText(tr("Close"));
		connect(m_leave, &QPushButton::clicked, this, &QWidget::close);
		return;
	}
	m_leave->setText(tr("Back"));
	connect(m_leave, &QPushButton::clicked, this, [this]() {
		m_runner.reset();
		if (m_session) {
			m_session->deleteLater();
		}
		m_endMessage.clear();
		m_started = false;
		for (QPushButton* b : m_startButtons) {
			b->setEnabled(true);
		}
		m_leave->setText(tr("Leave"));
		disconnect(m_leave, nullptr, nullptr, nullptr);
		connect(m_leave, &QPushButton::clicked, this, [this]() { endMatch(tr("You left the match.")); });
		m_setupStatus->setText(m_view->property("lastMessage").toString());
		m_pages->setCurrentIndex(0);
	});
	m_view->setProperty("lastMessage", message);
}

void NetplayWindow::refresh() {
	if (!m_runner) {
		return;
	}
	std::vector<uint32_t> pixels;
	int width, height;
	m_runner->videoSize(m_runner->localGba(), &width, &height);
	if (m_runner->picture(m_runner->localGba(), &pixels)) {
		m_view->setPicture(pixels, width, height);
	}
	if (m_runner->drawsAll()) {
		int view = 0;
		for (int gba = 0; gba < m_runner->gbas() && view < m_otherViews.size(); ++gba) {
			if (gba == m_runner->localGba()) {
				continue;
			}
			if (m_runner->picture(gba, &pixels)) {
				m_runner->videoSize(gba, &width, &height);
				m_otherViews[view]->setPicture(pixels, width, height);
			}
			++view;
		}
	}
	uint16_t keys = 0;
	if (!m_chatInput->hasFocus() && isActiveWindow()) {
		keys = m_keyboardKeys;
		if (m_input) {
			keys |= uint16_t(m_input->pollEvents());
		}
	}
	m_runner->setKeys(keys & 0x3FF);
	if (++m_tick % 15 == 0) {
		updatePanels();
		if (m_otherViews.size() != (m_runner->drawsAll() ? qMax(0, m_runner->gbas() - 1) : 0)) {
			updateOtherScreens();
		}
	}
}

void NetplayWindow::updatePanels() {
	NetplayRunner::Stats st;
	if (m_runner) {
		st = m_runner->stats();
	}
	int self = m_session ? m_session->self() : 0;
	QString players;
	for (int i = 0; i < m_playerNames.size(); ++i) {
		QString line = QStringLiteral("<span style='color:%1;font-weight:bold'>%2</span>&nbsp;&nbsp;%3")
		                   .arg(QString::fromLatin1(PLAYER_COLORS[i % 4]))
		                   .arg(i + 1)
		                   .arg(escape(m_playerNames[i]));
		if (m_mode != Mode::Online) {
			if (i > 0) {
				line += QStringLiteral(" <span style='color:#9a92b0'>%1</span>").arg(tr("(idle on this computer)"));
			}
		} else if (i == self) {
			line += QStringLiteral(" <span style='color:#ff4f9a'>%1</span>").arg(tr("you"));
		} else if (i < st.roundTripMs.size() && st.roundTripMs[i] >= 0) {
			line += QStringLiteral(" <span style='color:#9a92b0'>%1 ms</span>").arg(st.roundTripMs[i]);
		}
		players += line + QStringLiteral("<br>");
	}
	m_players->setText(players);
	QString connection;
	if (m_mode == Mode::Online) {
		connection = tr("Input delay %1  ·  Rollback last %2, largest %3  ·  Ahead %4  ·  Desyncs %5  ·  Frame %6")
		                 .arg(st.inputDelay)
		                 .arg(st.lastRollback)
		                 .arg(st.maxRollback)
		                 .arg(st.framesAhead, 0, 'f', 1)
		                 .arg(st.desyncs)
		                 .arg(qMax(0, st.frame));
	} else if (m_mode == Mode::StressTest) {
		connection = tr("Stress test: %1 rollbacks, %2 re-simulated frames, %3 desyncs  ·  Frame %4")
		                 .arg(st.rollbacks)
		                 .arg(st.resimFrames)
		                 .arg(st.desyncs)
		                 .arg(qMax(0, st.frame));
	} else {
		connection = tr("On this computer  ·  Frame %1").arg(qMax(0, st.frame));
	}
	m_connection->setText(connection);
}

void NetplayWindow::updateOtherScreens() {
	for (NetplayView* v : m_otherViews) {
		m_screensLayout->removeWidget(v);
		v->deleteLater();
	}
	m_otherViews.clear();
	if (!m_runner || !m_runner->drawsAll() || m_runner->gbas() < 2) {
		m_screens->hide();
		return;
	}
	int widest = 0;
	for (int gba = 0; gba < m_runner->gbas(); ++gba) {
		int width, height;
		m_runner->videoSize(gba, &width, &height);
		if (gba != m_runner->localGba()) {
			widest = qMax(widest, width);
		}
	}
	if (widest > 0) {
		// the panel shows the other screens at their own size at least
		m_side->setFixedWidth(qMax(320, widest + 24));
	}
	for (int gba = 0; gba < m_runner->gbas(); ++gba) {
		if (gba == m_runner->localGba()) {
			continue;
		}
		NetplayView* view = new NetplayView;
		int width, height;
		m_runner->videoSize(gba, &width, &height);
		if (width > 0) {
			view->setVideoSize(width, height);
		}
		view->setMinimumSize(view->minimumSizeHint());
		view->setCaption(tr("GBA %1").arg(gba + 1));
		view->setPlaceholder(tr("GBA %1").arg(gba + 1));
		m_screensLayout->addWidget(view);
		m_otherViews.append(view);
	}
	m_screens->show();
}

void NetplayWindow::appendChat(const QString& name, const QString& text) {
	if (name.isEmpty()) {
		m_chatLog->append(QStringLiteral("<span style='color:#9a92b0'>%1</span>").arg(escape(text)));
	} else {
		m_chatLog->append(QStringLiteral("<span style='color:#2ee6d6'>%1</span>&nbsp; %2").arg(escape(name), escape(text)));
	}
	m_chatLog->verticalScrollBar()->setValue(m_chatLog->verticalScrollBar()->maximum());
}

void NetplayWindow::sendChat() {
	QString text = m_chatInput->text().trimmed();
	if (!text.isEmpty() && m_session) {
		m_session->sendChat(text);
	}
	m_chatInput->clear();
	m_view->setFocus();
}

int NetplayWindow::gbaKey(int qtKey) const {
	return m_input ? m_input->mapKeyboard(qtKey) : -1;
}

int NetplayWindow::chatKey() const {
	if (gbaKey(Qt::Key_Return) < 0) {
		return Qt::Key_Return;
	}
	if (gbaKey(Qt::Key_T) < 0) {
		return Qt::Key_T;
	}
	return 0;   // both are GBA keys: the chat is typed in with a click
}

void NetplayWindow::updateKeyHints() {
	QStringList hints;
	int chat = chatKey();
	if (m_chatInput->isEnabled() && chat) {
		hints.append(tr("%1: chat").arg(chat == Qt::Key_Return ? tr("Enter") : QStringLiteral("T")));
	}
	if (gbaKey(Qt::Key_F1) < 0) {
		hints.append(tr("F1: panels"));
	}
	if (gbaKey(Qt::Key_F2) < 0) {
		hints.append(tr("F2: other screens"));
	}
	m_keyHints->setText(hints.join(QStringLiteral(" · ")));
}

void NetplayWindow::keyPressEvent(QKeyEvent* event) {
	if (event->isAutoRepeat()) {
		QWidget::keyPressEvent(event);
		return;
	}
	// the game's keys first, then the window's
	int key = gbaKey(event->key());
	if (key >= 0) {
		m_keyboardKeys |= 1 << key;
		event->accept();
		return;
	}
	if (event->key() == Qt::Key_F1) {
		m_side->setVisible(!m_side->isVisible());
		return;
	}
	if (event->key() == Qt::Key_F2 && m_runner) {
		m_runner->setDrawAll(!m_runner->drawsAll());
		return;
	}
	int chat = chatKey();
	if (chat && (event->key() == chat || (chat == Qt::Key_Return && event->key() == Qt::Key_Enter)) && m_chatInput->isEnabled()) {
		m_keyboardKeys = 0;
		m_side->show();
		m_chatInput->setFocus();
		return;
	}
	QWidget::keyPressEvent(event);
}

void NetplayWindow::keyReleaseEvent(QKeyEvent* event) {
	if (event->isAutoRepeat()) {
		QWidget::keyReleaseEvent(event);
		return;
	}
	int key = gbaKey(event->key());
	if (key < 0) {
		QWidget::keyReleaseEvent(event);
		return;
	}
	m_keyboardKeys &= ~(1 << key);
	event->accept();
}

bool NetplayWindow::eventFilter(QObject* object, QEvent* event) {
	if (object == m_chatInput && event->type() == QEvent::KeyPress) {
		QKeyEvent* key = static_cast<QKeyEvent*>(event);
		if (key->key() == Qt::Key_Escape) {
			m_chatInput->clear();
			m_view->setFocus();
			return true;
		}
	}
	return QWidget::eventFilter(object, event);
}

void NetplayWindow::closeEvent(QCloseEvent* event) {
	if (m_runner || m_session) {
		endMatch(tr("You left the match."));
	}
	QWidget::closeEvent(event);
	emit closed();
}
