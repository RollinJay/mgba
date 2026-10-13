/* SPDX-License-Identifier: MIT
 * Part of netplay-online (https://github.com/RollinJay/Nakama-Matchmaker). */
#include "MatchSession.h"

#include "PeerLinks.h"

#include <QJsonDocument>
#include <QJsonObject>

using namespace NetplayOnline;

MatchSession::MatchSession(QObject* parent)
	: QObject(parent) {
	qRegisterMetaType<NetplayOnline::MatchSession::State>();
	m_relay.setInterval(20);
	connect(&m_relay, &QTimer::timeout, this, &MatchSession::relaySignals);
}

MatchSession::~MatchSession() {
	m_relay.stop();
}

void MatchSession::setState(State state) {
	if (m_state == state) {
		return;
	}
	m_state = state;
	emit stateChanged(state);
}

void MatchSession::fail(const QString& error) {
	if (m_state == State::Failed || m_state == State::Ended) {
		return;
	}
	m_error = error;
	m_relay.stop();
	setState(State::Failed);
	emit failed(error);
}

int MatchSession::playerIndex(const QString& userId) const {
	for (int i = 0; i < m_players.size(); ++i) {
		if (m_players[i].userId == userId) {
			return i;
		}
	}
	return -1;
}

void MatchSession::startFromLauncher(const ConnectInfo& info) {
	m_client = new NakamaClient(info.server, this);
	m_client->useSession(info.token, info.refreshToken);
	m_players = info.players;
	m_self = info.self;
	m_name = info.players.value(info.self).name;
	m_identity = info.identity;
	m_kind = info.kind;
	m_stun = info.stunServers;
	m_port = info.port;
	m_pendingMatchId = info.matchId;
	m_pendingMatchToken = info.matchToken;
	connect(m_client, &NakamaClient::failed, this, &MatchSession::fail);
	connect(m_client, &NakamaClient::socketClosed, this, [this](const QString& reason) {
		if (m_state != State::Ended && m_state != State::Ready) {
			fail(QStringLiteral("disconnected from the server: %1").arg(reason));
		}
	});
	connect(m_client, &NakamaClient::socketOpened, this, [this]() {
		setState(State::Joining);
		m_client->joinMatch(m_pendingMatchId, m_pendingMatchToken);
	});
	connect(m_client, &NakamaClient::matchJoined, this, [this](const QString& matchId) { onMatchJoined(matchId); });
	connect(m_client, &NakamaClient::matchData, this, [this](const QString&, qint64 op, const QByteArray& data, const Presence& sender) {
		onMatchData(op, data, sender);
	});
	connect(m_client, &NakamaClient::matchPresence, this, [this](const QString&, const QList<Presence>&, const QList<Presence>& leaves) {
		for (const Presence& p : leaves) {
			int player = playerIndex(p.userId);
			if (player >= 0 && player != m_self) {
				emit playerLeft(player);
			}
		}
	});
	connect(m_client, &NakamaClient::serverError, this, [this](const QString& message) {
		if (m_state == State::Joining) {
			fail(message);
		}
	});
	setState(State::Connecting);
	m_client->connectSocket();
}

void MatchSession::startQuickMatch(const QuickMatch& q) {
	m_client = new NakamaClient(q.server, this);
	m_name = q.name;
	m_identity = q.identity;
	m_kind = q.kind;
	m_stun = q.stunServers;
	m_port = q.port;
	m_wantedPlayers = q.players;
	connect(m_client, &NakamaClient::failed, this, &MatchSession::fail);
	connect(m_client, &NakamaClient::signedIn, this, [this]() {
		setState(State::Connecting);
		m_client->connectSocket();
	});
	connect(m_client, &NakamaClient::socketClosed, this, [this](const QString& reason) {
		if (m_state != State::Ended && m_state != State::Ready) {
			fail(QStringLiteral("disconnected from the server: %1").arg(reason));
		}
	});
	connect(m_client, &NakamaClient::socketOpened, this, [this]() {
		setState(State::Matchmaking);
		QString players = QString::number(m_wantedPlayers);
		QVariantMap properties {
			{ QStringLiteral("game"), m_identity },
			{ QStringLiteral("kind"), m_kind },
			{ QStringLiteral("mode"), QStringLiteral("unranked") },
			{ QStringLiteral("players"), players },
		};
		QString query = QStringLiteral("+properties.game:%1 +properties.kind:%2 +properties.mode:unranked +properties.players:%3")
		                    .arg(m_identity, m_kind, players);
		m_client->matchmake(query, m_wantedPlayers, m_wantedPlayers, properties, QVariantMap());
	});
	connect(m_client, &NakamaClient::matchmakerMatched, this, [this](const Matched& m) {
		m_players.clear();
		for (const MatchedUser& u : m.users) {
			m_players.append(MatchPlayer { u.presence.userId, u.presence.username });
		}
		m_self = m.selfIndex();
		if (m_self >= 0) {
			m_players[m_self].name = m_name;
		}
		setState(State::Joining);
		m_client->joinMatch(m.matchId, m.token);
	});
	connect(m_client, &NakamaClient::matchJoined, this, [this](const QString& matchId) { onMatchJoined(matchId); });
	connect(m_client, &NakamaClient::matchData, this, [this](const QString&, qint64 op, const QByteArray& data, const Presence& sender) {
		onMatchData(op, data, sender);
	});
	connect(m_client, &NakamaClient::matchPresence, this, [this](const QString&, const QList<Presence>&, const QList<Presence>& leaves) {
		for (const Presence& p : leaves) {
			int player = playerIndex(p.userId);
			if (player >= 0 && player != m_self) {
				emit playerLeft(player);
			}
		}
	});
	connect(m_client, &NakamaClient::serverError, this, [this](const QString& message) {
		if (m_state == State::Matchmaking || m_state == State::Joining) {
			fail(message);
		}
	});
	setState(State::SigningIn);
	m_client->authenticateDevice(q.deviceId);
}

void MatchSession::onMatchJoined(const QString& matchId) {
	if (m_self < 0 || m_self >= m_players.size()) {
		fail(QStringLiteral("this player is not part of the match"));
		return;
	}
	QStringList peers;
	for (int i = 0; i < m_players.size(); ++i) {
		if (i != m_self) {
			peers.append(m_players[i].userId);
		}
	}
	m_links.reset(new PeerLinks(matchId, m_players[m_self].userId, peers));
	for (const QByteArray& signal : m_earlySignals) {
		m_links->pushSignal(signal);
	}
	m_earlySignals.clear();
	m_relay.start();
	QJsonObject hello {
		{ QStringLiteral("player"), m_self },
		{ QStringLiteral("name"), m_name },
		{ QStringLiteral("identity"), m_identity },
		{ QStringLiteral("kind"), m_kind },
	};
	m_client->sendMatchData(OP_HELLO, QJsonDocument(hello).toJson(QJsonDocument::Compact));
	setState(State::Linking);
	emit linking();
}

void MatchSession::onMatchData(qint64 op, const QByteArray& data, const Presence& sender) {
	switch (op) {
	case OP_SIGNAL:
		if (m_links) {
			m_links->pushSignal(data);
		} else {
			m_earlySignals.append(data);
		}
		break;
	case OP_HELLO: {
		QJsonObject o = QJsonDocument::fromJson(data).object();
		int player = playerIndex(sender.userId);
		if (player < 0) {
			break;
		}
		QString name = o.value(QStringLiteral("name")).toString().left(32);
		if (!name.isEmpty()) {
			m_players[player].name = name;
			emit playersChanged();
		}
		QString identity = o.value(QStringLiteral("identity")).toString();
		if (!m_identity.isEmpty() && !identity.isEmpty() && identity != m_identity) {
			fail(QStringLiteral("player %1 has a different game setup (emulator build, ROM or save)").arg(player + 1));
		}
		break;
	}
	case OP_CHAT: {
		QJsonObject o = QJsonDocument::fromJson(data).object();
		int player = playerIndex(sender.userId);
		QString name = player >= 0 ? m_players[player].name : sender.username;
		emit chatReceived(name, o.value(QStringLiteral("text")).toString().left(200));
		break;
	}
	default:
		break;
	}
}

void MatchSession::relaySignals() {
	if (!m_links || !m_client) {
		return;
	}
	for (const QByteArray& signal : m_links->takeSignals()) {
		m_client->sendMatchData(OP_SIGNAL, signal);
	}
}

void MatchSession::sendChat(const QString& text) {
	if (!m_client || m_client->currentMatchId().isEmpty() || text.trimmed().isEmpty()) {
		return;
	}
	QJsonObject o { { QStringLiteral("text"), text.left(200) } };
	m_client->sendMatchData(OP_CHAT, QJsonDocument(o).toJson(QJsonDocument::Compact));
	emit chatReceived(m_name, text.left(200));
}

void MatchSession::setLinked(bool ok, const QString& error) {
	if (!ok) {
		fail(error);
		return;
	}
	if (m_state == State::Linking) {
		setState(State::Ready);
	}
}

void MatchSession::leave() {
	m_relay.stop();
	if (m_client) {
		m_client->leaveMatch();
		m_client->disconnectSocket();
	}
	if (m_state != State::Failed) {
		setState(State::Ended);
	}
}
