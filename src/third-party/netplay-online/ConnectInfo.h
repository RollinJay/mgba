/* SPDX-License-Identifier: MIT
 * Part of netplay-online (https://github.com/RollinJay/Nakama-Matchmaker). */
#pragma once

#include "NakamaClient.h"

#include <QString>
#include <QStringList>

namespace NetplayOnline {

struct MatchPlayer {
	QString userId;
	QString name;
};

/* What the launcher hands an emulator for one online match: the server and a session to
 * use there, the match that the matchmaker made, the players in order and which one is
 * local, and the game. The launcher writes it to a file that only the user can read and
 * passes the file's path in the NETPLAY_CONNECT environment variable; the emulator reads
 * the file and deletes it, since it holds the session's tokens. docs/launch.md in the
 * Nakama-Matchmaker repository describes the format. */
struct ConnectInfo {
	ServerConfig server;
	QString token;
	QString refreshToken;
	QString matchId;
	QString matchToken;   // a relayed match (3 or 4 players with the IKEMEN fork's modules)
	QList<MatchPlayer> players;
	int self = -1;
	QString kind;         // "mgba", "melonds" or "ikemen"
	QString profile;      // the emulator's netplay profile
	QString identity;     // the profile's identity, which the matchmaker paired on
	QStringList stunServers;
	int port = 0;         // UDP port for the direct connections; 0 picks one
	int inputDelay = -1;  // frames; -1 picks one from the measured round trip

	bool write(const QString& path, QString* error) const;
	static bool read(const QString& path, ConnectInfo* out, QString* error);
	/* Reads and deletes the file named by NETPLAY_CONNECT; false when it is not set. */
	static bool fromEnvironment(ConnectInfo* out, QString* error);
};

}
