/* SPDX-License-Identifier: MIT
 * Part of netplay-online (https://github.com/RollinJay/Nakama-Matchmaker). */
#include "WebSocket.h"

#include <QCryptographicHash>
#include <QTcpSocket>
#ifndef QT_NO_SSL
#include <QSslSocket>
#endif

#include <cstring>
#include <random>

using namespace NetplayOnline;

namespace {

const int OP_CONTINUATION = 0x0;
const int OP_TEXT = 0x1;
const int OP_BINARY = 0x2;
const int OP_CLOSE = 0x8;
const int OP_PING = 0x9;
const int OP_PONG = 0xA;
const quint64 MAX_PAYLOAD = 64ull << 20;

QByteArray randomBytes(int count) {
	static std::random_device device;
	QByteArray out(count, Qt::Uninitialized);
	for (int i = 0; i < count; ++i) {
		out[i] = static_cast<char>(device() & 0xFF);
	}
	return out;
}

}

WebSocket::WebSocket(QObject* parent)
	: QObject(parent) {
}

WebSocket::~WebSocket() {
	if (m_socket) {
		m_socket->disconnect(this);
		m_socket->abort();
	}
}

void WebSocket::open(const QUrl& url) {
	if (m_socket) {
		m_socket->disconnect(this);
		m_socket->abort();
		m_socket->deleteLater();
		m_socket = nullptr;
	}
	m_url = url;
	m_buffer.clear();
	m_message.clear();
	m_closeReason.clear();
	bool secure = url.scheme() == QLatin1String("wss");
	int port = url.port(secure ? 443 : 80);
#ifndef QT_NO_SSL
	if (secure) {
		if (!QSslSocket::supportsSsl()) {
			m_state = State::Closed;
			emit closed(QStringLiteral("TLS is not available in this build"));
			return;
		}
		QSslSocket* ssl = new QSslSocket(this);
		m_socket = ssl;
		connect(ssl, &QSslSocket::encrypted, this, &WebSocket::onConnected);
	} else
#endif
	{
		if (secure) {
			m_state = State::Closed;
			emit closed(QStringLiteral("TLS is not available in this build"));
			return;
		}
		m_socket = new QTcpSocket(this);
		connect(m_socket, &QTcpSocket::connected, this, &WebSocket::onConnected);
	}
	connect(m_socket, &QTcpSocket::readyRead, this, &WebSocket::onReadyRead);
	connect(m_socket, &QTcpSocket::disconnected, this, &WebSocket::onDisconnected);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
	connect(m_socket, &QAbstractSocket::errorOccurred, this, &WebSocket::onError);
#else
	connect(m_socket, static_cast<void (QAbstractSocket::*)(QAbstractSocket::SocketError)>(&QAbstractSocket::error), this, &WebSocket::onError);
#endif
	m_state = State::Connecting;
#ifndef QT_NO_SSL
	if (secure) {
		static_cast<QSslSocket*>(m_socket)->connectToHostEncrypted(url.host(), port);
		return;
	}
#endif
	m_socket->connectToHost(url.host(), port);
}

void WebSocket::close() {
	if (!m_socket) {
		return;
	}
	if (m_state == State::Open) {
		m_state = State::Closing;
		QByteArray payload;
		payload.append(char(0x03));
		payload.append(char(0xE8));   // 1000: normal closure
		sendFrame(OP_CLOSE, payload);
		m_socket->flush();
	}
	m_closeReason = QStringLiteral("closed");
	m_socket->disconnectFromHost();
}

bool WebSocket::sendText(const QByteArray& utf8) {
	if (m_state != State::Open) {
		return false;
	}
	sendFrame(OP_TEXT, utf8);
	return true;
}

void WebSocket::onConnected() {
	m_state = State::Handshake;
	m_key = randomBytes(16).toBase64();
	int port = m_url.port(m_url.scheme() == QLatin1String("wss") ? 443 : 80);
	QByteArray path = m_url.path(QUrl::FullyEncoded).toUtf8();
	if (path.isEmpty()) {
		path = "/";
	}
	if (m_url.hasQuery()) {
		path += '?' + m_url.query(QUrl::FullyEncoded).toUtf8();
	}
	QByteArray request;
	request += "GET " + path + " HTTP/1.1\r\n";
	request += "Host: " + m_url.host().toUtf8() + ':' + QByteArray::number(port) + "\r\n";
	request += "Upgrade: websocket\r\n";
	request += "Connection: Upgrade\r\n";
	request += "Sec-WebSocket-Key: " + m_key + "\r\n";
	request += "Sec-WebSocket-Version: 13\r\n\r\n";
	m_socket->write(request);
}

void WebSocket::onReadyRead() {
	m_buffer += m_socket->readAll();
	if (m_state == State::Handshake) {
		if (!parseHandshake()) {
			return;
		}
	}
	if (m_state == State::Open || m_state == State::Closing) {
		parseFrames();
	}
}

bool WebSocket::parseHandshake() {
	int end = m_buffer.indexOf("\r\n\r\n");
	if (end < 0) {
		if (m_buffer.size() > 16384) {
			fail(QStringLiteral("invalid WebSocket handshake response"));
		}
		return false;
	}
	QByteArray head = m_buffer.left(end);
	m_buffer.remove(0, end + 4);
	QList<QByteArray> lines = head.split('\n');
	QByteArray status = lines.isEmpty() ? QByteArray() : lines.first().trimmed();
	if (!status.startsWith("HTTP/1.1 101")) {
		fail(QStringLiteral("WebSocket handshake refused: %1").arg(QString::fromUtf8(status)));
		return false;
	}
	QByteArray expected = QCryptographicHash::hash(m_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", QCryptographicHash::Sha1).toBase64();
	bool accepted = false;
	for (int i = 1; i < lines.size(); ++i) {
		QByteArray line = lines[i].trimmed();
		int colon = line.indexOf(':');
		if (colon < 0) {
			continue;
		}
		if (line.left(colon).trimmed().toLower() == "sec-websocket-accept") {
			accepted = line.mid(colon + 1).trimmed() == expected;
		}
	}
	if (!accepted) {
		fail(QStringLiteral("WebSocket handshake returned a wrong Sec-WebSocket-Accept"));
		return false;
	}
	m_state = State::Open;
	emit opened();
	return true;
}

void WebSocket::parseFrames() {
	while (m_socket && (m_state == State::Open || m_state == State::Closing)) {
		const uchar* p = reinterpret_cast<const uchar*>(m_buffer.constData());
		qint64 size = m_buffer.size();
		if (size < 2) {
			return;
		}
		bool fin = p[0] & 0x80;
		int opcode = p[0] & 0x0F;
		bool masked = p[1] & 0x80;
		quint64 length = p[1] & 0x7F;
		qint64 offset = 2;
		if (length == 126) {
			if (size < 4) {
				return;
			}
			length = (quint64(p[2]) << 8) | p[3];
			offset = 4;
		} else if (length == 127) {
			if (size < 10) {
				return;
			}
			length = 0;
			for (int i = 0; i < 8; ++i) {
				length = (length << 8) | p[2 + i];
			}
			offset = 10;
		}
		if (length > MAX_PAYLOAD) {
			fail(QStringLiteral("WebSocket message too large"));
			return;
		}
		uchar mask[4] = { 0, 0, 0, 0 };
		if (masked) {
			if (size < offset + 4) {
				return;
			}
			memcpy(mask, p + offset, 4);
			offset += 4;
		}
		if (quint64(size - offset) < length) {
			return;
		}
		QByteArray payload = m_buffer.mid(int(offset), int(length));
		m_buffer.remove(0, int(offset + qint64(length)));
		if (masked) {
			for (int i = 0; i < payload.size(); ++i) {
				payload[i] = char(payload[i] ^ mask[i & 3]);
			}
		}
		switch (opcode) {
		case OP_PING:
			sendFrame(OP_PONG, payload);
			break;
		case OP_PONG:
			break;
		case OP_CLOSE:
			if (m_state == State::Open) {
				m_state = State::Closing;
				sendFrame(OP_CLOSE, payload.left(2));
			}
			m_closeReason = payload.size() > 2 ? QString::fromUtf8(payload.mid(2)) : QStringLiteral("closed by the server");
			m_socket->disconnectFromHost();
			return;
		case OP_TEXT:
		case OP_BINARY:
			m_message = payload;
			m_messageOpcode = opcode;
			if (fin) {
				if (m_messageOpcode == OP_TEXT) {
					emit textReceived(m_message);
				}
				m_message.clear();
			}
			break;
		case OP_CONTINUATION:
			m_message += payload;
			if (quint64(m_message.size()) > MAX_PAYLOAD) {
				fail(QStringLiteral("WebSocket message too large"));
				return;
			}
			if (fin) {
				if (m_messageOpcode == OP_TEXT) {
					emit textReceived(m_message);
				}
				m_message.clear();
			}
			break;
		default:
			fail(QStringLiteral("unknown WebSocket opcode %1").arg(opcode));
			return;
		}
	}
}

void WebSocket::sendFrame(int opcode, const QByteArray& payload) {
	if (!m_socket) {
		return;
	}
	QByteArray frame;
	frame.reserve(payload.size() + 14);
	frame.append(char(0x80 | opcode));
	quint64 length = payload.size();
	if (length < 126) {
		frame.append(char(0x80 | length));
	} else if (length <= 0xFFFF) {
		frame.append(char(0x80 | 126));
		frame.append(char((length >> 8) & 0xFF));
		frame.append(char(length & 0xFF));
	} else {
		frame.append(char(0x80 | 127));
		for (int i = 7; i >= 0; --i) {
			frame.append(char((length >> (8 * i)) & 0xFF));
		}
	}
	QByteArray mask = randomBytes(4);
	frame.append(mask);
	int start = frame.size();
	frame.append(payload);
	for (int i = 0; i < payload.size(); ++i) {
		frame[start + i] = char(frame[start + i] ^ mask[i & 3]);
	}
	m_socket->write(frame);
}

void WebSocket::fail(const QString& reason) {
	m_closeReason = reason;
	if (m_socket) {
		m_socket->abort();
	}
	if (m_state != State::Closed) {
		m_state = State::Closed;
		emit closed(reason);
	}
}

void WebSocket::onDisconnected() {
	if (m_state == State::Closed) {
		return;
	}
	m_state = State::Closed;
	emit closed(m_closeReason.isEmpty() ? QStringLiteral("connection closed") : m_closeReason);
}

void WebSocket::onError(QAbstractSocket::SocketError) {
	if (m_state == State::Closed || m_state == State::Closing) {
		return;
	}
	QString reason = m_socket ? m_socket->errorString() : QStringLiteral("socket error");
	fail(reason);
}
