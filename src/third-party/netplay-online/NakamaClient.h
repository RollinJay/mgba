/* SPDX-License-Identifier: MIT
 * Part of netplay-online (https://github.com/RollinJay/Nakama-Matchmaker). */
#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

namespace NetplayOnline {

class WebSocket;

struct Presence {
	QString userId;
	QString sessionId;
	QString username;
};

struct MatchedUser {
	Presence presence;
	QVariantMap properties;   // string and numeric properties together
};

/* A matchmaker result. Nakama sends the users in the same order to every one of them,
 * so the order numbers the players. Either matchId (a server-run match created by the
 * server's matchmaker hook) or token (a relayed match) is set. */
struct Matched {
	QString ticket;
	QString matchId;
	QString token;
	QList<MatchedUser> users;
	MatchedUser self;
	int selfIndex() const;
};

struct Session {
	QString token;
	QString refreshToken;
	QString userId;
	QString username;
	qint64 expires = 0;   // seconds since 1970
	bool valid() const { return !token.isEmpty(); }
};

struct ServerConfig {
	QString host = QStringLiteral("127.0.0.1");
	int port = 7350;
	QString serverKey = QStringLiteral("defaultkey");
	bool ssl = false;
};

/* A Nakama client: device sign-in and session refresh over REST, remote procedure calls,
 * and the realtime socket (matchmaking, matches, chat). It belongs to the thread that
 * runs the Qt event loop it was created in. Its protocol follows the IKEMEN GO Nakama
 * fork's client, so that the same server modules serve both. */
class NakamaClient : public QObject {
	Q_OBJECT

public:
	using RpcCallback = std::function<void(bool ok, const QByteArray& reply, const QString& error)>;

	explicit NakamaClient(const ServerConfig&, QObject* parent = nullptr);
	~NakamaClient() override;

	const ServerConfig& server() const { return m_server; }
	const Session& session() const { return m_session; }

	/* Signs in with a device id, creating the account on first use, and emits signedIn()
	 * or failed(). A username, if not empty, is the name other players see: a new account
	 * takes it, and an existing account with another name is renamed. Usernames are unique
	 * on the server, so when another account has the name, this account gets the name with
	 * a tag made from the device id ("Ryu#3fa0"), the same at every sign-in; if that is
	 * taken as well, a new account gets a name from the server and an existing one keeps
	 * its name. Whitespace in the name becomes '_' (the server refuses it). */
	void authenticateDevice(const QString& deviceId, const QString& username = QString());
	static QString taggedName(const QString& username, const QString& deviceId);
	/* Uses a session obtained elsewhere (from the launcher); refreshes it when needed. */
	void useSession(const QString& token, const QString& refreshToken);
	void rpc(const QString& id, const QJsonObject& payload, RpcCallback);

	/* The realtime socket. Emits socketOpened() or socketClosed(). */
	void connectSocket();
	void disconnectSocket();
	bool isSocketOpen() const;

	void matchmake(const QString& query, int minCount, int maxCount, const QVariantMap& stringProperties,
	               const QVariantMap& numericProperties);
	void cancelMatchmaking();
	QString matchmakerTicket() const { return m_ticket; }

	void joinMatch(const QString& matchId, const QString& token = QString(), const QVariantMap& metadata = QVariantMap());
	void leaveMatch();
	QString currentMatchId() const { return m_matchId; }
	void sendMatchData(qint64 opCode, const QByteArray& data, bool reliable = true);

	void joinRoom(const QString& roomName);
	void leaveChannel(const QString& channelId);
	void sendChat(const QString& channelId, const QString& text);

	void ping();
	qint64 serverRoundTripMs() const { return m_rttMs; }

signals:
	void signedIn();
	void failed(const QString& error);
	void socketOpened();
	void socketClosed(const QString& reason);
	void serverError(const QString& message);
	void matchmakerTicketReceived(const QString& ticket);
	void matchmakerMatched(const NetplayOnline::Matched&);
	void matchJoined(const QString& matchId, const QList<NetplayOnline::Presence>& presences, const NetplayOnline::Presence& self);
	void matchPresence(const QString& matchId, const QList<NetplayOnline::Presence>& joins, const QList<NetplayOnline::Presence>& leaves);
	void matchData(const QString& matchId, qint64 opCode, const QByteArray& data, const NetplayOnline::Presence& sender);
	void channelJoined(const QString& channelId, const QString& roomName, const QList<NetplayOnline::Presence>& presences);
	void channelPresence(const QString& channelId, const QList<NetplayOnline::Presence>& joins, const QList<NetplayOnline::Presence>& leaves);
	void chatMessage(const QString& channelId, const QString& senderId, const QString& username, const QString& text);
	void pong(qint64 roundTripMs);

private slots:
	void onSocketText(const QByteArray&);

private:
	QString baseUrl(bool websocket) const;
	bool parseSession(const QByteArray& json, Session* out, QString* error) const;
	void refreshThen(std::function<void(bool ok, const QString& error)>);
	using HttpCallback = std::function<void(int status, const QByteArray& body, const QString& error)>;
	void post(const QString& path, const QByteArray& body, bool basicAuth, HttpCallback);
	void put(const QString& path, const QByteArray& body, HttpCallback);
	void request(const QByteArray& verb, const QString& path, const QByteArray& body, bool basicAuth, HttpCallback);
	void signInAttempt(const QString& name, int attempt);
	void renameAttempt(const QString& name, int attempt);
	void send(const QString& field, const QJsonObject& payload, const QString& cid = QString());

	ServerConfig m_server;
	Session m_session;
	QNetworkAccessManager* m_http = nullptr;
	WebSocket* m_socket = nullptr;
	QString m_deviceId;
	QString m_ticket;
	QString m_matchId;
	int m_cid = 0;
	QHash<QString, qint64> m_pings;
	QElapsedTimer m_clock;
	qint64 m_rttMs = -1;
};

}

Q_DECLARE_METATYPE(NetplayOnline::Presence)
Q_DECLARE_METATYPE(NetplayOnline::Matched)
