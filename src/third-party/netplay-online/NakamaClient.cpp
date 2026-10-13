/* SPDX-License-Identifier: MIT
 * Part of netplay-online (https://github.com/RollinJay/Nakama-Matchmaker). */
#include "NakamaClient.h"

#include "WebSocket.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

#include <memory>

using namespace NetplayOnline;

namespace {

Presence parsePresence(const QJsonObject& o) {
	Presence p;
	p.userId = o.value(QStringLiteral("user_id")).toString();
	p.sessionId = o.value(QStringLiteral("session_id")).toString();
	p.username = o.value(QStringLiteral("username")).toString();
	return p;
}

QList<Presence> parsePresences(const QJsonValue& v) {
	QList<Presence> out;
	for (const QJsonValue& item : v.toArray()) {
		out.append(parsePresence(item.toObject()));
	}
	return out;
}

MatchedUser parseMatchedUser(const QJsonObject& o) {
	MatchedUser u;
	u.presence = parsePresence(o.value(QStringLiteral("presence")).toObject());
	QJsonObject strings = o.value(QStringLiteral("string_properties")).toObject();
	for (auto it = strings.begin(); it != strings.end(); ++it) {
		u.properties.insert(it.key(), it.value().toString());
	}
	QJsonObject numbers = o.value(QStringLiteral("numeric_properties")).toObject();
	for (auto it = numbers.begin(); it != numbers.end(); ++it) {
		u.properties.insert(it.key(), it.value().toDouble());
	}
	return u;
}

/* protojson writes int64 fields as strings */
qint64 toInt64(const QJsonValue& v) {
	if (v.isString()) {
		return v.toString().toLongLong();
	}
	return qint64(v.toDouble());
}

}

int Matched::selfIndex() const {
	for (int i = 0; i < users.size(); ++i) {
		if (users[i].presence.sessionId == self.presence.sessionId) {
			return i;
		}
	}
	return -1;
}

NakamaClient::NakamaClient(const ServerConfig& server, QObject* parent)
	: QObject(parent)
	, m_server(server)
	, m_http(new QNetworkAccessManager(this)) {
	qRegisterMetaType<NetplayOnline::Presence>();
	qRegisterMetaType<NetplayOnline::Matched>();
	m_clock.start();
}

NakamaClient::~NakamaClient() {
	disconnectSocket();
}

QString NakamaClient::baseUrl(bool websocket) const {
	QString scheme = websocket ? (m_server.ssl ? QStringLiteral("wss") : QStringLiteral("ws"))
	                           : (m_server.ssl ? QStringLiteral("https") : QStringLiteral("http"));
	return QStringLiteral("%1://%2:%3").arg(scheme, m_server.host).arg(m_server.port);
}

void NakamaClient::post(const QString& path, const QByteArray& body, bool basicAuth, HttpCallback done) {
	request("POST", path, body, basicAuth, done);
}

void NakamaClient::put(const QString& path, const QByteArray& body, HttpCallback done) {
	request("PUT", path, body, false, done);
}

void NakamaClient::request(const QByteArray& verb, const QString& path, const QByteArray& body, bool basicAuth, HttpCallback done) {
	QNetworkRequest request(QUrl(baseUrl(false) + path));
	request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
	if (basicAuth) {
		request.setRawHeader("Authorization", "Basic " + (m_server.serverKey.toUtf8() + ':').toBase64());
	} else {
		request.setRawHeader("Authorization", "Bearer " + m_session.token.toUtf8());
	}
	QNetworkReply* reply = m_http->sendCustomRequest(request, verb, body);
	connect(reply, &QNetworkReply::finished, this, [reply, done]() {
		int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		QByteArray data = reply->readAll();
		QString error;
		if (reply->error() != QNetworkReply::NoError && status == 0) {
			error = reply->errorString();
		}
		reply->deleteLater();
		done(status, data, error);
	});
}

bool NakamaClient::parseSession(const QByteArray& json, Session* out, QString* error) const {
	QJsonObject o = QJsonDocument::fromJson(json).object();
	Session s;
	s.token = o.value(QStringLiteral("token")).toString();
	s.refreshToken = o.value(QStringLiteral("refresh_token")).toString();
	if (s.token.isEmpty()) {
		*error = QStringLiteral("the server returned no session token");
		return false;
	}
	QList<QByteArray> parts = s.token.toUtf8().split('.');
	if (parts.size() != 3) {
		*error = QStringLiteral("the session token is not a JWT");
		return false;
	}
	QByteArray payload = QByteArray::fromBase64(parts[1], QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
	QJsonObject claims = QJsonDocument::fromJson(payload).object();
	s.userId = claims.value(QStringLiteral("uid")).toString();
	s.username = claims.value(QStringLiteral("usn")).toString();
	s.expires = qint64(claims.value(QStringLiteral("exp")).toDouble());
	*out = s;
	return true;
}

QString NakamaClient::taggedName(const QString& username, const QString& deviceId) {
	QByteArray tag = QCryptographicHash::hash(deviceId.toUtf8(), QCryptographicHash::Sha1).toHex().left(4);
	return username + QLatin1Char('#') + QString::fromLatin1(tag);
}

void NakamaClient::authenticateDevice(const QString& deviceId, const QString& username) {
	m_deviceId = deviceId;
	QString name = username.trimmed();
	name.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral("_"));
	signInAttempt(name, name.isEmpty() ? 2 : 0);
}

/* attempt 0 asks for the name, 1 for the tagged name and 2 for none (the server makes one
 * up for a new account) */
void NakamaClient::signInAttempt(const QString& name, int attempt) {
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("create"), QStringLiteral("true"));
	QString requested = attempt == 0 ? name : attempt == 1 ? taggedName(name, m_deviceId) : QString();
	if (!requested.isEmpty()) {
		query.addQueryItem(QStringLiteral("username"), requested);
	}
	QJsonObject body { { QStringLiteral("id"), m_deviceId } };
	post(QStringLiteral("/v2/account/authenticate/device?") + query.toString(QUrl::FullyEncoded),
	     QJsonDocument(body).toJson(QJsonDocument::Compact), true,
	     [this, name, attempt](int status, const QByteArray& data, const QString& error) {
		if ((status == 409 || status == 400) && attempt < 2) {
			// a new account, and another account has the name (409), or the server refused
			// the name (400)
			signInAttempt(name, attempt + 1);
			return;
		}
		if (status < 200 || status >= 300) {
			emit failed(error.isEmpty() ? QStringLiteral("sign-in failed: HTTP %1 %2").arg(status).arg(QString::fromUtf8(data).trimmed()) : error);
			return;
		}
		QString parseError;
		if (!parseSession(data, &m_session, &parseError)) {
			emit failed(parseError);
			return;
		}
		if (attempt == 0 && m_session.username != name && m_session.username != taggedName(name, m_deviceId)) {
			// an existing account with another name
			renameAttempt(name, 0);
			return;
		}
		emit signedIn();
	});
}

void NakamaClient::renameAttempt(const QString& name, int attempt) {
	QString requested = attempt == 0 ? name : taggedName(name, m_deviceId);
	QJsonObject body { { QStringLiteral("username"), requested } };
	put(QStringLiteral("/v2/account"), QJsonDocument(body).toJson(QJsonDocument::Compact),
	    [this, name, attempt](int status, const QByteArray&, const QString&) {
		if (attempt == 0 && (status == 409 || status == 400)) {
			// another account has the name (Nakama 3.41 answers 400 "Username is already in use")
			renameAttempt(name, 1);
			return;
		}
		if (status >= 200 && status < 300) {
			// the session names the old name: sign in again for one with the new name
			signInAttempt(name, 2);
			return;
		}
		// the name is taken (or the server refused it): keep the account's name
		emit signedIn();
	});
}

void NakamaClient::useSession(const QString& token, const QString& refreshToken) {
	QJsonObject o { { QStringLiteral("token"), token }, { QStringLiteral("refresh_token"), refreshToken } };
	QString error;
	parseSession(QJsonDocument(o).toJson(QJsonDocument::Compact), &m_session, &error);
}

void NakamaClient::refreshThen(std::function<void(bool, const QString&)> next) {
	qint64 now = QDateTime::currentMSecsSinceEpoch() / 1000;
	if (!m_session.valid()) {
		next(false, QStringLiteral("not signed in"));
		return;
	}
	if (m_session.expires == 0 || now + 30 < m_session.expires) {
		next(true, QString());
		return;
	}
	QJsonObject body { { QStringLiteral("token"), m_session.refreshToken } };
	post(QStringLiteral("/v2/account/session/refresh"), QJsonDocument(body).toJson(QJsonDocument::Compact), true,
	     [this, next](int status, const QByteArray& data, const QString& error) {
		if (status < 200 || status >= 300) {
			if (!m_deviceId.isEmpty()) {
				// the refresh token expired as well: sign in again
				auto connection = std::make_shared<QMetaObject::Connection>();
				auto failure = std::make_shared<QMetaObject::Connection>();
				*connection = connect(this, &NakamaClient::signedIn, this, [this, next, connection, failure]() {
					disconnect(*connection);
					disconnect(*failure);
					next(true, QString());
				});
				*failure = connect(this, &NakamaClient::failed, this, [this, next, connection, failure](const QString& e) {
					disconnect(*connection);
					disconnect(*failure);
					next(false, e);
				});
				authenticateDevice(m_deviceId);
				return;
			}
			next(false, error.isEmpty() ? QStringLiteral("session refresh failed: HTTP %1").arg(status) : error);
			return;
		}
		Session refreshed;
		QString parseError;
		if (!parseSession(data, &refreshed, &parseError)) {
			next(false, parseError);
			return;
		}
		if (refreshed.refreshToken.isEmpty()) {
			refreshed.refreshToken = m_session.refreshToken;
		}
		m_session = refreshed;
		next(true, QString());
	});
}

void NakamaClient::rpc(const QString& id, const QJsonObject& payload, RpcCallback done) {
	refreshThen([this, id, payload, done](bool ok, const QString& error) {
		if (!ok) {
			done(false, QByteArray(), error);
			return;
		}
		// ?unwrap sends and receives the payload as raw JSON
		post(QStringLiteral("/v2/rpc/%1?unwrap").arg(QString::fromUtf8(QUrl::toPercentEncoding(id))),
		     QJsonDocument(payload).toJson(QJsonDocument::Compact), false,
		     [done](int status, const QByteArray& data, const QString& netError) {
			if (status < 200 || status >= 300) {
				QString message = QJsonDocument::fromJson(data).object().value(QStringLiteral("message")).toString();
				if (message.isEmpty()) {
					message = netError.isEmpty() ? QStringLiteral("HTTP %1").arg(status) : netError;
				}
				done(false, data, message);
				return;
			}
			done(true, data, QString());
		});
	});
}

void NakamaClient::connectSocket() {
	refreshThen([this](bool ok, const QString& error) {
		if (!ok) {
			emit failed(error);
			return;
		}
		if (m_socket) {
			m_socket->disconnect(this);
			m_socket->deleteLater();
		}
		m_socket = new WebSocket(this);
		connect(m_socket, &WebSocket::opened, this, &NakamaClient::socketOpened);
		connect(m_socket, &WebSocket::closed, this, [this](const QString& reason) {
			m_ticket.clear();
			m_matchId.clear();
			emit socketClosed(reason);
		});
		connect(m_socket, &WebSocket::textReceived, this, &NakamaClient::onSocketText);
		QUrlQuery query;
		query.addQueryItem(QStringLiteral("lang"), QStringLiteral("en"));
		query.addQueryItem(QStringLiteral("status"), QStringLiteral("true"));
		query.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
		query.addQueryItem(QStringLiteral("token"), m_session.token);
		QUrl url(baseUrl(true) + QStringLiteral("/ws"));
		url.setQuery(query);
		m_socket->open(url);
	});
}

void NakamaClient::disconnectSocket() {
	if (m_socket) {
		m_socket->close();
	}
}

bool NakamaClient::isSocketOpen() const {
	return m_socket && m_socket->isOpen();
}

void NakamaClient::send(const QString& field, const QJsonObject& payload, const QString& cid) {
	if (!isSocketOpen()) {
		emit serverError(QStringLiteral("not connected to the server"));
		return;
	}
	QJsonObject envelope;
	envelope.insert(QStringLiteral("cid"), cid.isEmpty() ? QString::number(++m_cid) : cid);
	envelope.insert(field, payload);
	m_socket->sendText(QJsonDocument(envelope).toJson(QJsonDocument::Compact));
}

void NakamaClient::matchmake(const QString& query, int minCount, int maxCount, const QVariantMap& stringProperties,
                             const QVariantMap& numericProperties) {
	QJsonObject strings;
	for (auto it = stringProperties.begin(); it != stringProperties.end(); ++it) {
		strings.insert(it.key(), it.value().toString());
	}
	QJsonObject numbers;
	for (auto it = numericProperties.begin(); it != numericProperties.end(); ++it) {
		numbers.insert(it.key(), it.value().toDouble());
	}
	QJsonObject add {
		{ QStringLiteral("query"), query },
		{ QStringLiteral("min_count"), minCount },
		{ QStringLiteral("max_count"), maxCount },
		{ QStringLiteral("count_multiple"), 1 },
		{ QStringLiteral("string_properties"), strings },
		{ QStringLiteral("numeric_properties"), numbers },
	};
	send(QStringLiteral("matchmaker_add"), add);
}

void NakamaClient::cancelMatchmaking() {
	if (m_ticket.isEmpty()) {
		return;
	}
	send(QStringLiteral("matchmaker_remove"), QJsonObject { { QStringLiteral("ticket"), m_ticket } });
	m_ticket.clear();
}

void NakamaClient::joinMatch(const QString& matchId, const QString& token, const QVariantMap& metadata) {
	QJsonObject join;
	if (!token.isEmpty()) {
		join.insert(QStringLiteral("token"), token);
	} else {
		join.insert(QStringLiteral("match_id"), matchId);
	}
	if (!metadata.isEmpty()) {
		QJsonObject meta;
		for (auto it = metadata.begin(); it != metadata.end(); ++it) {
			meta.insert(it.key(), it.value().toString());
		}
		join.insert(QStringLiteral("metadata"), meta);
	}
	send(QStringLiteral("match_join"), join);
}

void NakamaClient::leaveMatch() {
	if (m_matchId.isEmpty()) {
		return;
	}
	send(QStringLiteral("match_leave"), QJsonObject { { QStringLiteral("match_id"), m_matchId } });
	m_matchId.clear();
}

void NakamaClient::sendMatchData(qint64 opCode, const QByteArray& data, bool reliable) {
	if (m_matchId.isEmpty()) {
		emit serverError(QStringLiteral("not in a match"));
		return;
	}
	QJsonObject message {
		{ QStringLiteral("match_id"), m_matchId },
		{ QStringLiteral("op_code"), QString::number(opCode) },
		{ QStringLiteral("data"), QString::fromLatin1(data.toBase64()) },
		{ QStringLiteral("reliable"), reliable },
	};
	send(QStringLiteral("match_data_send"), message);
}

void NakamaClient::joinRoom(const QString& roomName) {
	QJsonObject join {
		{ QStringLiteral("target"), roomName },
		{ QStringLiteral("type"), 1 },   // room
		{ QStringLiteral("persistence"), true },
		{ QStringLiteral("hidden"), false },
	};
	send(QStringLiteral("channel_join"), join);
}

void NakamaClient::leaveChannel(const QString& channelId) {
	send(QStringLiteral("channel_leave"), QJsonObject { { QStringLiteral("channel_id"), channelId } });
}

void NakamaClient::sendChat(const QString& channelId, const QString& text) {
	QJsonObject content { { QStringLiteral("text"), text } };
	QJsonObject message {
		{ QStringLiteral("channel_id"), channelId },
		{ QStringLiteral("content"), QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Compact)) },
	};
	send(QStringLiteral("channel_message_send"), message);
}

void NakamaClient::ping() {
	QString cid = QStringLiteral("ping-%1").arg(++m_cid);
	m_pings.insert(cid, m_clock.elapsed());
	send(QStringLiteral("ping"), QJsonObject(), cid);
}

void NakamaClient::onSocketText(const QByteArray& text) {
	QJsonObject envelope = QJsonDocument::fromJson(text).object();
	if (envelope.isEmpty()) {
		return;
	}
	QString cid = envelope.value(QStringLiteral("cid")).toString();
	if (envelope.contains(QStringLiteral("match_data"))) {
		QJsonObject o = envelope.value(QStringLiteral("match_data")).toObject();
		QByteArray data = QByteArray::fromBase64(o.value(QStringLiteral("data")).toString().toLatin1());
		emit matchData(o.value(QStringLiteral("match_id")).toString(), toInt64(o.value(QStringLiteral("op_code"))), data,
		               parsePresence(o.value(QStringLiteral("presence")).toObject()));
	} else if (envelope.contains(QStringLiteral("match_presence_event"))) {
		QJsonObject o = envelope.value(QStringLiteral("match_presence_event")).toObject();
		emit matchPresence(o.value(QStringLiteral("match_id")).toString(), parsePresences(o.value(QStringLiteral("joins"))),
		                   parsePresences(o.value(QStringLiteral("leaves"))));
	} else if (envelope.contains(QStringLiteral("matchmaker_ticket"))) {
		m_ticket = envelope.value(QStringLiteral("matchmaker_ticket")).toObject().value(QStringLiteral("ticket")).toString();
		emit matchmakerTicketReceived(m_ticket);
	} else if (envelope.contains(QStringLiteral("matchmaker_matched"))) {
		QJsonObject o = envelope.value(QStringLiteral("matchmaker_matched")).toObject();
		Matched m;
		m.ticket = o.value(QStringLiteral("ticket")).toString();
		m.matchId = o.value(QStringLiteral("match_id")).toString();
		m.token = o.value(QStringLiteral("token")).toString();
		for (const QJsonValue& u : o.value(QStringLiteral("users")).toArray()) {
			m.users.append(parseMatchedUser(u.toObject()));
		}
		m.self = parseMatchedUser(o.value(QStringLiteral("self")).toObject());
		m_ticket.clear();
		emit matchmakerMatched(m);
	} else if (envelope.contains(QStringLiteral("match"))) {
		QJsonObject o = envelope.value(QStringLiteral("match")).toObject();
		m_matchId = o.value(QStringLiteral("match_id")).toString();
		emit matchJoined(m_matchId, parsePresences(o.value(QStringLiteral("presences"))),
		                 parsePresence(o.value(QStringLiteral("self")).toObject()));
	} else if (envelope.contains(QStringLiteral("channel"))) {
		QJsonObject o = envelope.value(QStringLiteral("channel")).toObject();
		emit channelJoined(o.value(QStringLiteral("id")).toString(), o.value(QStringLiteral("room_name")).toString(),
		                   parsePresences(o.value(QStringLiteral("presences"))));
	} else if (envelope.contains(QStringLiteral("channel_presence_event"))) {
		QJsonObject o = envelope.value(QStringLiteral("channel_presence_event")).toObject();
		emit channelPresence(o.value(QStringLiteral("channel_id")).toString(), parsePresences(o.value(QStringLiteral("joins"))),
		                     parsePresences(o.value(QStringLiteral("leaves"))));
	} else if (envelope.contains(QStringLiteral("channel_message"))) {
		QJsonObject o = envelope.value(QStringLiteral("channel_message")).toObject();
		QJsonObject content = QJsonDocument::fromJson(o.value(QStringLiteral("content")).toString().toUtf8()).object();
		emit chatMessage(o.value(QStringLiteral("channel_id")).toString(), o.value(QStringLiteral("sender_id")).toString(),
		                 o.value(QStringLiteral("username")).toString(), content.value(QStringLiteral("text")).toString());
	} else if (envelope.contains(QStringLiteral("pong"))) {
		auto it = m_pings.find(cid);
		if (it != m_pings.end()) {
			m_rttMs = m_clock.elapsed() - it.value();
			m_pings.erase(it);
			emit pong(m_rttMs);
		}
	} else if (envelope.contains(QStringLiteral("error"))) {
		QJsonObject o = envelope.value(QStringLiteral("error")).toObject();
		emit serverError(QStringLiteral("server error %1: %2").arg(o.value(QStringLiteral("code")).toInt()).arg(o.value(QStringLiteral("message")).toString()));
	}
}
