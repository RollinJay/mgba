/* SPDX-License-Identifier: MIT
 * Part of netplay-online (https://github.com/RollinJay/Nakama-Matchmaker). */
#include "PeerLinks.h"

#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QNetworkInterface>
#include <QThread>

#include <algorithm>
#include <cstring>
#include <random>

using namespace NetplayOnline;

const char* const PeerLinks::MAGIC = "IKEMEN-P2P/1";

namespace {

const qint64 PUNCH_INTERVAL_MS = 75;
const qint64 RESIGNAL_INTERVAL_MS = 500;
const qint64 HANDSHAKE_TIMEOUT_MS = 20000;
const qint64 PING_INTERVAL_MS = 1000;
const qint64 KEEPALIVE_INTERVAL_MS = 2000;
const qint64 SILENCE_TIMEOUT_MS = 8000;
const int STUN_TIMEOUT_MS = 3000;
const int STUN_RETRY_MS = 500;

const quint8 TAG_CONTROL = 0xA0;
const quint8 TAG_STREAM = 0xA1;   // the fork's reliable stream; not used here
const quint8 TAG_GAME = 0xA2;
const quint8 CTRL_KEEPALIVE = 0x01;
const quint8 CTRL_BYE = 0x02;
const quint8 CTRL_PING = 0x03;
const quint8 CTRL_PONG = 0x04;

const quint16 STUN_BINDING_REQUEST = 0x0001;
const quint16 STUN_BINDING_SUCCESS = 0x0101;
const quint16 STUN_MAPPED_ADDRESS = 0x0001;
const quint16 STUN_XOR_MAPPED_ADDRESS = 0x0020;
const quint32 STUN_MAGIC_COOKIE = 0x2112A442;

quint16 be16(const uchar* p) {
	return quint16((p[0] << 8) | p[1]);
}

quint32 be32(const uchar* p) {
	return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | p[3];
}

QByteArray randomHex(int bytes) {
	std::random_device device;
	QByteArray raw(bytes, Qt::Uninitialized);
	for (int i = 0; i < bytes; ++i) {
		raw[i] = char(device() & 0xFF);
	}
	return raw.toHex();
}

bool parseStunResponse(const uchar* p, int length, const uchar* transaction, Address* mapped) {
	if (length < 20 || be16(p) != STUN_BINDING_SUCCESS || be32(p + 4) != STUN_MAGIC_COOKIE || memcmp(p + 8, transaction, 12) != 0) {
		return false;
	}
	int bodyEnd = 20 + be16(p + 2);
	if (bodyEnd > length) {
		return false;
	}
	for (int offset = 20; offset + 4 <= bodyEnd;) {
		quint16 type = be16(p + offset);
		int attrLength = be16(p + offset + 2);
		int value = offset + 4;
		if (value + attrLength > length) {
			return false;
		}
		if ((type == STUN_XOR_MAPPED_ADDRESS || type == STUN_MAPPED_ADDRESS) && attrLength >= 8 && p[value + 1] == 0x01) {
			quint16 port = be16(p + value + 2);
			quint32 ip = be32(p + value + 4);
			if (type == STUN_XOR_MAPPED_ADDRESS) {
				port ^= quint16(STUN_MAGIC_COOKIE >> 16);
				ip ^= STUN_MAGIC_COOKIE;
			}
			mapped->ip = ip;
			mapped->port = port;
			return true;
		}
		offset = value + attrLength + ((4 - (attrLength % 4)) % 4);
	}
	return false;
}

/* One STUN binding request, sent again every half second, for up to 3 seconds. */
bool stunQuery(UdpSocket& socket, const Address& server, Address* mapped) {
	uchar request[20];
	uchar transaction[12];
	std::random_device device;
	for (int i = 0; i < 12; ++i) {
		transaction[i] = uchar(device() & 0xFF);
	}
	request[0] = STUN_BINDING_REQUEST >> 8;
	request[1] = STUN_BINDING_REQUEST & 0xFF;
	request[2] = 0;
	request[3] = 0;
	request[4] = STUN_MAGIC_COOKIE >> 24;
	request[5] = (STUN_MAGIC_COOKIE >> 16) & 0xFF;
	request[6] = (STUN_MAGIC_COOKIE >> 8) & 0xFF;
	request[7] = STUN_MAGIC_COOKIE & 0xFF;
	memcpy(request + 8, transaction, 12);
	QElapsedTimer clock;
	clock.start();
	char buffer[2048];
	while (clock.elapsed() < STUN_TIMEOUT_MS) {
		socket.sendTo(server, reinterpret_cast<const char*>(request), sizeof(request));
		qint64 retry = clock.elapsed() + STUN_RETRY_MS;
		while (clock.elapsed() < retry) {
			if (!socket.wait(int(retry - clock.elapsed()))) {
				continue;
			}
			Address from;
			int n;
			while ((n = socket.receive(buffer, sizeof(buffer), &from)) >= 0) {
				if (parseStunResponse(reinterpret_cast<const uchar*>(buffer), n, transaction, mapped)) {
					return true;
				}
			}
		}
	}
	return false;
}

bool resolve(const QString& hostPort, Address* out) {
	int colon = hostPort.lastIndexOf(QLatin1Char(':'));
	if (colon <= 0) {
		return false;
	}
	QString host = hostPort.left(colon).trimmed();
	int port = hostPort.mid(colon + 1).toInt();
	if (Address::parse(host, port, out)) {
		return true;
	}
	QHostInfo info = QHostInfo::fromName(host);
	for (const QHostAddress& a : info.addresses()) {
		if (a.protocol() == QAbstractSocket::IPv4Protocol) {
			out->ip = a.toIPv4Address();
			out->port = quint16(port);
			return true;
		}
	}
	return false;
}

}

PeerLinks::PeerLinks(const QString& matchId, const QString& selfId, const QStringList& peerIds)
	: m_matchId(matchId)
	, m_selfId(selfId)
	, m_token(QString::fromLatin1(randomHex(16))) {
	for (const QString& id : peerIds) {
		Peer p;
		p.id = id;
		m_peers.append(p);
	}
	m_clock.start();
}

PeerLinks::~PeerLinks() {
	close();
}

bool PeerLinks::start(quint16 port, const QStringList& stunServers, QString* error) {
	if (!m_socket.open(port, error)) {
		QMutexLocker locker(&m_lock);
		m_failed = true;
		m_error = *error;
		return false;
	}
	QList<Candidate> local;
	QStringList seen;
	for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
		QNetworkInterface::InterfaceFlags flags = iface.flags();
		if (!(flags & QNetworkInterface::IsUp) || (flags & QNetworkInterface::IsLoopBack)) {
			continue;
		}
		for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
			QHostAddress a = entry.ip();
			if (a.protocol() != QAbstractSocket::IPv4Protocol || a.isLoopback()) {
				continue;
			}
			QString text = a.toString();
			if (!seen.contains(text)) {
				seen.append(text);
			}
		}
	}
	std::sort(seen.begin(), seen.end());
	if (seen.isEmpty()) {
		seen.append(QStringLiteral("127.0.0.1"));   // a computer without a network: local play only
	}
	for (const QString& ip : seen) {
		local.append(Candidate { QStringLiteral("host"), ip, m_socket.localPort() });
	}
	for (const QString& server : stunServers) {
		Address stun;
		if (server.trimmed().isEmpty() || !resolve(server.trimmed(), &stun)) {
			continue;
		}
		Address mapped;
		if (stunQuery(m_socket, stun, &mapped)) {
			QString ip = mapped.toString().section(QLatin1Char(':'), 0, 0);
			local.append(Candidate { QStringLiteral("srflx"), ip, mapped.port });
			break;
		}
	}
	{
		QMutexLocker locker(&m_lock);
		m_local = local;
	}
	m_deadline = m_clock.elapsed() + HANDSHAKE_TIMEOUT_MS;
	m_started = true;
	return true;
}

QByteArray PeerLinks::signalFor(const Peer& peer) const {
	QJsonArray candidates;
	for (const Candidate& c : m_local) {
		candidates.append(QJsonObject {
			{ QStringLiteral("type"), c.type },
			{ QStringLiteral("address"), c.address },
			{ QStringLiteral("port"), c.port },
		});
	}
	QJsonObject signal {
		{ QStringLiteral("magic"), QString::fromLatin1(MAGIC) },
		{ QStringLiteral("match_id"), m_matchId },
		{ QStringLiteral("token"), m_token },
		{ QStringLiteral("candidates"), candidates },
		{ QStringLiteral("from"), m_selfId },
		{ QStringLiteral("to"), peer.id },
	};
	return QJsonDocument(signal).toJson(QJsonDocument::Compact);
}

QByteArray PeerLinks::handshake(const char* kind) const {
	QJsonObject packet {
		{ QStringLiteral("magic"), QString::fromLatin1(MAGIC) },
		{ QStringLiteral("kind"), QString::fromLatin1(kind) },
		{ QStringLiteral("match_id"), m_matchId },
		{ QStringLiteral("token"), m_token },
	};
	return QJsonDocument(packet).toJson(QJsonDocument::Compact);
}

void PeerLinks::pushSignal(const QByteArray& json) {
	QMutexLocker locker(&m_lock);
	m_inbox.append(json);
}

QList<QByteArray> PeerLinks::takeSignals() {
	QMutexLocker locker(&m_lock);
	QList<QByteArray> out = m_outbox;
	m_outbox.clear();
	return out;
}

void PeerLinks::applySignal(const QByteArray& json) {
	QJsonObject o = QJsonDocument::fromJson(json).object();
	if (o.value(QStringLiteral("magic")).toString() != QLatin1String(MAGIC) ||
	    o.value(QStringLiteral("match_id")).toString() != m_matchId) {
		return;
	}
	QString token = o.value(QStringLiteral("token")).toString();
	QString to = o.value(QStringLiteral("to")).toString();
	if (token.isEmpty() || token == m_token || (!to.isEmpty() && to != m_selfId)) {
		return;   // our own signal echoed back, or one meant for another player
	}
	int peer = peerIndex(o.value(QStringLiteral("from")).toString());
	if (peer < 0) {
		return;
	}
	QList<Address> candidates;
	for (const QJsonValue& v : o.value(QStringLiteral("candidates")).toArray()) {
		QJsonObject c = v.toObject();
		Address a;
		if (Address::parse(c.value(QStringLiteral("address")).toString(), c.value(QStringLiteral("port")).toInt(), &a) && !candidates.contains(a)) {
			candidates.append(a);
		}
	}
	QMutexLocker locker(&m_lock);
	m_peers[peer].token = token;
	m_peers[peer].candidates = candidates;
}

int PeerLinks::peerIndex(const QString& id) const {
	for (int i = 0; i < m_peers.size(); ++i) {
		if (m_peers[i].id == id) {
			return i;
		}
	}
	return -1;
}

int PeerLinks::peerByAddress(const Address& from) const {
	for (int i = 0; i < m_peers.size(); ++i) {
		if (m_peers[i].connected && m_peers[i].known.contains(from)) {
			return i;
		}
	}
	return -1;
}

void PeerLinks::markConnected(int peer, const Address& from) {
	QMutexLocker locker(&m_lock);
	Peer& p = m_peers[peer];
	if (!p.known.contains(from)) {
		p.known.append(from);
	}
	if (!p.connected) {
		p.connected = true;
		p.address = from;
		p.lastReceive = m_clock.elapsed();
	}
}

void PeerLinks::handleHandshake(const QByteArray& data, const Address& from) {
	QJsonObject o = QJsonDocument::fromJson(data).object();
	if (o.value(QStringLiteral("magic")).toString() != QLatin1String(MAGIC) ||
	    o.value(QStringLiteral("match_id")).toString() != m_matchId) {
		return;
	}
	QString token = o.value(QStringLiteral("token")).toString();
	QString kind = o.value(QStringLiteral("kind")).toString();
	int peer = -1;
	{
		QMutexLocker locker(&m_lock);
		for (int i = 0; i < m_peers.size(); ++i) {
			if (!token.isEmpty() && m_peers[i].token == token) {
				peer = i;
				break;
			}
		}
	}
	if (peer < 0) {
		return;   // a peer whose signal has not arrived yet: it keeps punching
	}
	if (kind == QLatin1String("hello")) {
		QByteArray ack = handshake("ack");
		// acks are sent redundantly; a later hello is answered again
		for (int i = 0; i < 3; ++i) {
			m_socket.sendTo(from, ack.constData(), ack.size());
		}
		markConnected(peer, from);
	} else if (kind == QLatin1String("ack")) {
		markConnected(peer, from);
	}
}

void PeerLinks::handleControl(int peer, const char* data, int length) {
	if (length < 1) {
		return;
	}
	quint8 type = quint8(data[0]);
	if (type == CTRL_PING && length >= 9) {
		QByteArray pong(data, 9);
		pong[0] = char(CTRL_PONG);
		sendTagged(peer, TAG_CONTROL, pong);
	} else if (type == CTRL_PONG && length >= 9) {
		quint64 sent = 0;
		for (int i = 7; i >= 0; --i) {
			sent = (sent << 8) | quint8(data[1 + i]);
		}
		qint64 sample = qint64(quint64(m_clock.nsecsElapsed() / 1000) - sent);
		if (sample > 0 && sample < 10000000) {
			QMutexLocker locker(&m_lock);
			Peer& p = m_peers[peer];
			p.srttUs = p.rttSamples ? p.srttUs + (sample - p.srttUs) / 8 : sample;
			++p.rttSamples;
		}
	} else if (type == CTRL_BYE) {
		QMutexLocker locker(&m_lock);
		m_peers[peer].lost = true;
	}
}

void PeerLinks::sendTagged(int peer, quint8 tag, const QByteArray& payload) {
	Peer& p = m_peers[peer];
	QByteArray packet;
	packet.reserve(payload.size() + 1);
	packet.append(char(tag));
	packet.append(payload);
	m_socket.sendTo(p.address, packet.constData(), packet.size());
	p.lastSend = m_clock.elapsed();
}

bool PeerLinks::sendGame(int peer, const char* data, int length) {
	if (peer < 0 || peer >= m_peers.size() || !m_peers[peer].connected || length > 4096) {
		return false;
	}
	char packet[4097];
	packet[0] = char(TAG_GAME);
	memcpy(packet + 1, data, length);
	bool ok = m_socket.sendTo(m_peers[peer].address, packet, length + 1);
	m_peers[peer].lastSend = m_clock.elapsed();
	return ok;
}

void PeerLinks::takeGamePackets(QVector<QPair<int, QByteArray>>* out) {
	out->append(m_games);
	m_games.clear();
}

void PeerLinks::poll() {
	if (!m_started || !m_socket.isOpen()) {
		return;
	}
	QList<QByteArray> inbox;
	{
		QMutexLocker locker(&m_lock);
		inbox = m_inbox;
		m_inbox.clear();
	}
	for (const QByteArray& signal : inbox) {
		applySignal(signal);
	}

	char buffer[8192];
	Address from;
	int n;
	int budget = 512;
	while (budget-- > 0 && (n = m_socket.receive(buffer, sizeof(buffer), &from)) >= 0) {
		if (n == 0) {
			continue;
		}
		quint8 first = quint8(buffer[0]);
		if (first == '{') {
			handleHandshake(QByteArray(buffer, n), from);
			continue;
		}
		if (first != TAG_CONTROL && first != TAG_GAME && first != TAG_STREAM) {
			continue;
		}
		int peer = peerByAddress(from);
		if (peer < 0) {
			continue;
		}
		m_peers[peer].lastReceive = m_clock.elapsed();
		if (first == TAG_CONTROL) {
			handleControl(peer, buffer + 1, n - 1);
		} else if (first == TAG_GAME) {
			m_games.append(qMakePair(peer, QByteArray(buffer + 1, n - 1)));
		}
	}

	qint64 now = m_clock.elapsed();
	bool waiting = false;
	for (int i = 0; i < m_peers.size(); ++i) {
		Peer& p = m_peers[i];
		if (!p.connected) {
			waiting = true;
			if (now - p.lastSignal >= RESIGNAL_INTERVAL_MS) {
				// the match relays only to players present when it is sent: keep sending
				// until the handshake completes
				QByteArray signal = signalFor(p);
				QMutexLocker locker(&m_lock);
				m_outbox.append(signal);
				p.lastSignal = now;
			}
			if (!p.token.isEmpty() && now - p.lastPunch >= PUNCH_INTERVAL_MS) {
				QByteArray hello = handshake("hello");
				for (const Address& a : p.candidates) {
					m_socket.sendTo(a, hello.constData(), hello.size());
				}
				p.lastPunch = now;
			}
			continue;
		}
		if (p.lost) {
			continue;
		}
		if (now - p.lastPing >= PING_INTERVAL_MS) {
			QByteArray ping(9, 0);
			ping[0] = char(CTRL_PING);
			quint64 stamp = quint64(m_clock.nsecsElapsed() / 1000);
			for (int b = 0; b < 8; ++b) {
				ping[1 + b] = char((stamp >> (8 * b)) & 0xFF);
			}
			sendTagged(i, TAG_CONTROL, ping);
			p.lastPing = now;
		}
		if (now - p.lastSend >= KEEPALIVE_INTERVAL_MS) {
			sendTagged(i, TAG_CONTROL, QByteArray(1, char(CTRL_KEEPALIVE)));
		}
		if (now - p.lastReceive >= SILENCE_TIMEOUT_MS) {
			QMutexLocker locker(&m_lock);
			p.lost = true;
		}
	}
	if (waiting && now > m_deadline) {
		QMutexLocker locker(&m_lock);
		if (!m_failed) {
			QStringList missing;
			for (const Peer& p : m_peers) {
				if (!p.connected) {
					missing.append(p.id);
				}
			}
			m_failed = true;
			m_error = QStringLiteral("could not open a connection to %1 within %2 seconds")
			              .arg(missing.join(QStringLiteral(", ")))
			              .arg(HANDSHAKE_TIMEOUT_MS / 1000);
		}
	}
}

bool PeerLinks::connected(int peer) const {
	QMutexLocker locker(&m_lock);
	return m_peers[peer].connected;
}

bool PeerLinks::allConnected() const {
	QMutexLocker locker(&m_lock);
	for (const Peer& p : m_peers) {
		if (!p.connected) {
			return false;
		}
	}
	return true;
}

bool PeerLinks::lost(int peer) const {
	QMutexLocker locker(&m_lock);
	return m_peers[peer].lost;
}

int PeerLinks::roundTripMs(int peer) const {
	QMutexLocker locker(&m_lock);
	const Peer& p = m_peers[peer];
	return p.rttSamples ? int((p.srttUs + 500) / 1000) : -1;
}

QString PeerLinks::peerAddress(int peer) const {
	QMutexLocker locker(&m_lock);
	return m_peers[peer].connected ? m_peers[peer].address.toString() : QString();
}

bool PeerLinks::failed() const {
	QMutexLocker locker(&m_lock);
	return m_failed;
}

QString PeerLinks::error() const {
	QMutexLocker locker(&m_lock);
	return m_error;
}

quint16 PeerLinks::localPort() const {
	return m_socket.localPort();
}

QList<Candidate> PeerLinks::localCandidates() const {
	QMutexLocker locker(&m_lock);
	return m_local;
}

void PeerLinks::close() {
	if (!m_socket.isOpen()) {
		return;
	}
	QByteArray bye(5, 0);
	bye[0] = char(CTRL_BYE);
	for (int i = 0; i < m_peers.size(); ++i) {
		if (m_peers[i].connected && !m_peers[i].lost) {
			for (int k = 0; k < 3; ++k) {
				sendTagged(i, TAG_CONTROL, bye);
			}
		}
	}
	m_socket.close();
}
