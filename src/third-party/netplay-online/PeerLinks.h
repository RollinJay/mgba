/* SPDX-License-Identifier: MIT
 * Part of netplay-online (https://github.com/RollinJay/Nakama-Matchmaker). */
#pragma once

#include "UdpSocket.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QMutex>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

namespace NetplayOnline {

struct Candidate {
	QString type;   // "host" or "srflx" (the public address that STUN reported)
	QString address;
	int port = 0;
};

/* Direct connections to every other player of a match, on one UDP socket.
 *
 * The players exchange address candidates through the Nakama match ("signals"), then
 * send each other handshake packets until one gets through both NATs ("hole punching").
 * After the handshake, the first byte of every datagram says what it carries: control
 * messages (keepalives, pings that measure the round trip, goodbye) or game packets.
 * The signals, the handshake and the byte tags follow the IKEMEN GO Nakama fork's P2P
 * layer (IKEMEN-P2P/1), extended from one peer to several on the same socket.
 *
 * One thread owns the object and calls start() and poll(); the signal queues and the
 * status getters may be used from any thread. */
class PeerLinks {
public:
	static const char* const MAGIC;

	PeerLinks(const QString& matchId, const QString& selfId, const QStringList& peerIds);
	~PeerLinks();

	/* Opens the socket (the given port, or an ephemeral one when it is taken) and
	 * gathers candidates: this computer's IPv4 addresses, and the public address from the
	 * first STUN server ("host:port") that answers. Blocks up to 3 seconds per server. */
	bool start(quint16 port, const QStringList& stunServers, QString* error);

	/* Signals from the other players, as received through the match (any thread). */
	void pushSignal(const QByteArray& json);
	/* Signals to publish through the match (any thread). */
	QList<QByteArray> takeSignals();

	/* Owner thread: reads the socket, punches, signals again, pings. */
	void poll();

	int peers() const { return m_peers.size(); }
	QString peerId(int peer) const { return m_peers[peer].id; }
	int peerIndex(const QString& id) const;

	bool connected(int peer) const;
	bool allConnected() const;
	bool lost(int peer) const;   // said goodbye, or silent for 8 seconds after connecting
	int roundTripMs(int peer) const;   // -1 until measured
	QString peerAddress(int peer) const;
	bool failed() const;
	QString error() const;
	quint16 localPort() const;
	QList<Candidate> localCandidates() const;

	/* Game packets (owner thread). */
	bool sendGame(int peer, const char* data, int length);
	void takeGamePackets(QVector<QPair<int, QByteArray>>* out);

	/* Says goodbye to every peer and closes the socket (owner thread). */
	void close();

private:
	struct Peer {
		QString id;
		QString token;   // from its signal
		QList<Address> candidates;
		bool connected = false;
		bool lost = false;
		Address address;   // where its handshake came from
		QList<Address> known;
		qint64 lastSignal = -1000000;
		qint64 lastPunch = -1000000;
		qint64 lastPing = 0;
		qint64 lastSend = 0;
		qint64 lastReceive = 0;
		qint64 srttUs = 0;
		int rttSamples = 0;
	};

	QByteArray signalFor(const Peer&) const;
	QByteArray handshake(const char* kind) const;
	void handleHandshake(const QByteArray& data, const Address& from);
	void handleControl(int peer, const char* data, int length);
	void markConnected(int peer, const Address& from);
	int peerByAddress(const Address& from) const;
	void sendTagged(int peer, quint8 tag, const QByteArray& payload);
	void applySignal(const QByteArray& json);

	QString m_matchId;
	QString m_selfId;
	QString m_token;
	QVector<Peer> m_peers;
	UdpSocket m_socket;
	QList<Candidate> m_local;
	QElapsedTimer m_clock;
	qint64 m_deadline = 0;
	bool m_started = false;

	mutable QMutex m_lock;   // guards the fields below and the status of the peers
	QList<QByteArray> m_inbox;
	QList<QByteArray> m_outbox;
	QVector<QPair<int, QByteArray>> m_games;
	bool m_failed = false;
	QString m_error;
};

}
