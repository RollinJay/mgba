/* SPDX-License-Identifier: MIT
 * Part of netplay-online (https://github.com/RollinJay/Nakama-Matchmaker). */
#include "ConnectInfo.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace NetplayOnline;

bool ConnectInfo::write(const QString& path, QString* error) const {
	QJsonArray playerList;
	for (const MatchPlayer& p : players) {
		playerList.append(QJsonObject { { QStringLiteral("user_id"), p.userId }, { QStringLiteral("name"), p.name } });
	}
	QJsonObject o {
		{ QStringLiteral("version"), 1 },
		{ QStringLiteral("server"), QJsonObject {
			{ QStringLiteral("host"), server.host },
			{ QStringLiteral("port"), server.port },
			{ QStringLiteral("key"), server.serverKey },
			{ QStringLiteral("ssl"), server.ssl },
		} },
		{ QStringLiteral("session"), QJsonObject {
			{ QStringLiteral("token"), token },
			{ QStringLiteral("refresh_token"), refreshToken },
		} },
		{ QStringLiteral("match"), QJsonObject {
			{ QStringLiteral("match_id"), matchId },
			{ QStringLiteral("token"), matchToken },
		} },
		{ QStringLiteral("players"), playerList },
		{ QStringLiteral("self"), self },
		{ QStringLiteral("game"), QJsonObject {
			{ QStringLiteral("kind"), kind },
			{ QStringLiteral("profile"), profile },
			{ QStringLiteral("identity"), identity },
		} },
		{ QStringLiteral("stun"), QJsonArray::fromStringList(stunServers) },
		{ QStringLiteral("port"), port },
		{ QStringLiteral("input_delay"), inputDelay },
	};
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		*error = file.errorString();
		return false;
	}
	file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
	file.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
	return true;
}

bool ConnectInfo::read(const QString& path, ConnectInfo* out, QString* error) {
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		*error = QStringLiteral("cannot read %1: %2").arg(path, file.errorString());
		return false;
	}
	QJsonParseError parseError;
	QJsonObject o = QJsonDocument::fromJson(file.readAll(), &parseError).object();
	if (parseError.error != QJsonParseError::NoError || o.value(QStringLiteral("version")).toInt() != 1) {
		*error = QStringLiteral("%1 is not a version 1 connection file").arg(path);
		return false;
	}
	ConnectInfo c;
	QJsonObject server = o.value(QStringLiteral("server")).toObject();
	c.server.host = server.value(QStringLiteral("host")).toString();
	c.server.port = server.value(QStringLiteral("port")).toInt(7350);
	c.server.serverKey = server.value(QStringLiteral("key")).toString(QStringLiteral("defaultkey"));
	c.server.ssl = server.value(QStringLiteral("ssl")).toBool();
	QJsonObject session = o.value(QStringLiteral("session")).toObject();
	c.token = session.value(QStringLiteral("token")).toString();
	c.refreshToken = session.value(QStringLiteral("refresh_token")).toString();
	QJsonObject match = o.value(QStringLiteral("match")).toObject();
	c.matchId = match.value(QStringLiteral("match_id")).toString();
	c.matchToken = match.value(QStringLiteral("token")).toString();
	for (const QJsonValue& v : o.value(QStringLiteral("players")).toArray()) {
		QJsonObject p = v.toObject();
		c.players.append(MatchPlayer { p.value(QStringLiteral("user_id")).toString(), p.value(QStringLiteral("name")).toString() });
	}
	c.self = o.value(QStringLiteral("self")).toInt(-1);
	QJsonObject game = o.value(QStringLiteral("game")).toObject();
	c.kind = game.value(QStringLiteral("kind")).toString();
	c.profile = game.value(QStringLiteral("profile")).toString();
	c.identity = game.value(QStringLiteral("identity")).toString();
	for (const QJsonValue& v : o.value(QStringLiteral("stun")).toArray()) {
		c.stunServers.append(v.toString());
	}
	c.port = o.value(QStringLiteral("port")).toInt();
	c.inputDelay = o.value(QStringLiteral("input_delay")).toInt(-1);
	if (c.server.host.isEmpty() || c.token.isEmpty() || (c.matchId.isEmpty() && c.matchToken.isEmpty()) ||
	    c.self < 0 || c.self >= c.players.size()) {
		*error = QStringLiteral("%1 is missing the server, session, match or players").arg(path);
		return false;
	}
	*out = c;
	return true;
}

bool ConnectInfo::fromEnvironment(ConnectInfo* out, QString* error) {
	QByteArray path = qgetenv("NETPLAY_CONNECT");
	if (path.isEmpty()) {
		return false;
	}
	QString file = QString::fromLocal8Bit(path);
	bool ok = read(file, out, error);
	QFile::remove(file);
	qunsetenv("NETPLAY_CONNECT");
	return ok;
}
