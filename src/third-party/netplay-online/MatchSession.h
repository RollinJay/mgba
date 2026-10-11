/* SPDX-License-Identifier: MIT
 * Part of netplay-online (https://github.com/RollinJay/Nakama-Matchmaker). */
#pragma once

#include "ConnectInfo.h"
#include "NakamaClient.h"

#include <QObject>
#include <QTimer>

#include <memory>

namespace NetplayOnline {

class PeerLinks;

/* Match data operation codes. Signals use the IKEMEN GO Nakama fork's code; the server
 * modules relay every code except the fork's rematch codes. */
enum : qint64 {
	OP_SIGNAL = 0x494B5347,   // address candidates for the direct connections
	OP_HELLO = 0x4E504845,    // a player's name and game identity, once joined
	OP_CHAT = 0x4E504348,     // a chat line inside the match
};

/* One online match from the emulator's side, on the thread that runs the Qt event loop:
 * it joins the match (one that the launcher found, or one it finds itself through the
 * matchmaker), tells the other players who it is, relays the signals of the direct
 * connections, and carries the match's chat.
 *
 * The direct connections themselves (links()) belong to the emulator's netplay thread,
 * which calls PeerLinks::start() and PeerLinks::poll(). */
class MatchSession : public QObject {
	Q_OBJECT

public:
	enum class State { Idle, SigningIn, Connecting, Matchmaking, Joining, Linking, Ready, Ended, Failed };

	struct QuickMatch {
		ServerConfig server;
		QString deviceId;
		QString name;
		QString kind;        // "mgba" or "melonds"
		QString identity;    // the profile's identity: only equal identities are paired
		int players = 2;
		QStringList stunServers;
		int port = 0;
	};

	explicit MatchSession(QObject* parent = nullptr);
	~MatchSession() override;

	/* Joins the match that the launcher found. */
	void startFromLauncher(const ConnectInfo&);
	/* Signs in and finds a match with the matchmaker. */
	void startQuickMatch(const QuickMatch&);
	void leave();

	State state() const { return m_state; }
	QString error() const { return m_error; }
	NakamaClient* client() const { return m_client; }

	/* Once Linking: the players in order (player 1 first) and the local one. */
	const QList<MatchPlayer>& players() const { return m_players; }
	int self() const { return m_self; }
	QString identity() const { return m_identity; }
	QStringList stunServers() const { return m_stun; }
	int port() const { return m_port; }
	/* The direct connections, for the netplay thread; valid until the session ends. */
	PeerLinks* links() const { return m_links.get(); }

	void sendChat(const QString& text);
	/* The netplay thread reports that every connection is open, or that they failed. */
	void setLinked(bool ok, const QString& error = QString());

signals:
	void stateChanged(NetplayOnline::MatchSession::State);
	void linking();   // links() exists: the netplay thread can start it
	void playersChanged();
	void chatReceived(const QString& name, const QString& text);
	void playerLeft(int player);
	void failed(const QString& error);

private:
	void setState(State);
	void fail(const QString&);
	void onMatchJoined(const QString& matchId);
	void onMatchData(qint64 op, const QByteArray& data, const Presence& sender);
	void relaySignals();
	int playerIndex(const QString& userId) const;

	NakamaClient* m_client = nullptr;
	State m_state = State::Idle;
	QString m_error;
	QString m_name;
	QString m_identity;
	QString m_kind;
	QStringList m_stun;
	int m_port = 0;
	int m_wantedPlayers = 2;
	QList<MatchPlayer> m_players;
	int m_self = -1;
	QString m_pendingMatchId;
	QString m_pendingMatchToken;
	std::unique_ptr<PeerLinks> m_links;
	QList<QByteArray> m_earlySignals;
	QTimer m_relay;
};

}

Q_DECLARE_METATYPE(NetplayOnline::MatchSession::State)
