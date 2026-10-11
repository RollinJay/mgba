/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include "NetplayRunner.h"

#include <ConnectInfo.h>

#include <mgba/core/log.h>

#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <memory>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTextEdit;
class QVBoxLayout;

namespace NetplayOnline {
class MatchSession;
}

namespace QGBA {

class ConfigController;
class InputController;
class LogController;
class NetplayAudio;
class NetplayView;

/* The netplay window: a setup page (profile, server, name; play on this computer, run a
 * stress test, or find a match), and the match screen: the game picture scaled by a whole
 * number, with panels beside it for the players, the connection and the match's chat.
 * Keys mapped to the GBA go to the game first. Of the others, F1 hides the panels, F2
 * shows or hides the other players' screens, and the chat key (Enter, or T when Enter is
 * a GBA key, as Start is by default) types in the chat, the game seeing every key let go
 * meanwhile; Escape returns to the game.
 *
 * It is a separate top-level window owned by the main window, whose keyboard mapping,
 * gamepads and settings it uses, so it closes with that window. */
class NetplayWindow : public QWidget {
	Q_OBJECT

public:
	NetplayWindow(ConfigController*, InputController*, LogController*, QWidget* parent = nullptr);
	~NetplayWindow() override;

	/* Joins the match that a launcher found (see NETPLAY_CONNECT). After the match the
	 * window offers to close, not to start another: the launcher makes the next one. */
	void startFromLauncher(const NetplayOnline::ConnectInfo&);

signals:
	void closed();

protected:
	void keyPressEvent(QKeyEvent*) override;
	void keyReleaseEvent(QKeyEvent*) override;
	void closeEvent(QCloseEvent*) override;
	bool eventFilter(QObject*, QEvent*) override;

private:
	enum class Mode { Local, StressTest, Online };

	QWidget* buildSetupPage();
	QWidget* buildMatchPage();
	void browseProfile();
	void saveSettings();
	bool loadProfileInfo(const QString& path, QString* error);
	void startLocal(bool stressTest);
	void startOnline();
	NetplayOnline::MatchSession* createSession(int inputDelay);
	void startRunner(NetplayRunner::Options);
	void beginMatchScreen(const QString& subtitle);
	void endMatch(const QString& message);
	void refresh();
	void updatePanels();
	void updateOtherScreens();
	void appendChat(const QString& name, const QString& text);
	void sendChat();
	int gbaKey(int qtKey) const;
	int chatKey() const;
	void updateKeyHints();
	QString deviceId();

	ConfigController* m_config;
	InputController* m_input;
	// the GBAs' log messages go to the main window's log, through its filter
	struct RunnerLogger : mLogger {
		LogController* controller;
	} m_logger{};
	Mode m_mode = Mode::Local;

	QStackedWidget* m_pages = nullptr;
	QLineEdit* m_profile = nullptr;
	QLineEdit* m_host = nullptr;
	QSpinBox* m_port = nullptr;
	QLineEdit* m_key = nullptr;
	QCheckBox* m_ssl = nullptr;
	QLineEdit* m_name = nullptr;
	QLabel* m_setupStatus = nullptr;
	QList<QPushButton*> m_startButtons;

	QLabel* m_title = nullptr;
	NetplayView* m_view = nullptr;
	QWidget* m_side = nullptr;
	QLabel* m_players = nullptr;
	QWidget* m_screens = nullptr;
	QVBoxLayout* m_screensLayout = nullptr;
	QList<NetplayView*> m_otherViews;
	QTextEdit* m_chatLog = nullptr;
	QLineEdit* m_chatInput = nullptr;
	QLabel* m_connection = nullptr;
	QLabel* m_keyHints = nullptr;
	QPushButton* m_leave = nullptr;

	QString m_profilePath;
	QString m_profileName;
	QString m_identity;
	int m_profilePlayers = 0;
	bool m_profileAll = false;
	QStringList m_playerNames;

	std::unique_ptr<NetplayRunner> m_runner;
	QPointer<NetplayOnline::MatchSession> m_session;
	NetplayAudio* m_audio = nullptr;
	QObject* m_audioOutput = nullptr;
	QTimer m_refresh;
	uint16_t m_keyboardKeys = 0;
	int m_tick = 0;
	bool m_started = false;
	bool m_fromLauncher = false;
	QString m_endMessage;
};

}
