/* SPDX-License-Identifier: MIT
 * Part of netplay-online (https://github.com/RollinJay/Nakama-Matchmaker). */
#pragma once

#include <QAbstractSocket>
#include <QByteArray>
#include <QObject>
#include <QUrl>

class QTcpSocket;

namespace NetplayOnline {

/* A WebSocket client (RFC 6455) for Nakama's realtime API, on Qt Network alone: text
 * messages, fragmented messages, ping and close. wss:// needs a Qt built with TLS. */
class WebSocket : public QObject {
	Q_OBJECT

public:
	explicit WebSocket(QObject* parent = nullptr);
	~WebSocket() override;

	void open(const QUrl& url);
	void close();
	bool isOpen() const { return m_state == State::Open; }
	bool sendText(const QByteArray& utf8);

signals:
	void opened();
	void closed(const QString& reason);
	void textReceived(const QByteArray& utf8);

private slots:
	void onConnected();
	void onReadyRead();
	void onDisconnected();
	void onError(QAbstractSocket::SocketError);

private:
	enum class State { Closed, Connecting, Handshake, Open, Closing };

	void sendFrame(int opcode, const QByteArray& payload);
	void fail(const QString& reason);
	bool parseHandshake();
	void parseFrames();

	QTcpSocket* m_socket = nullptr;
	QUrl m_url;
	QByteArray m_key;
	QByteArray m_buffer;
	QByteArray m_message;   // fragments of a message being received
	int m_messageOpcode = 0;
	State m_state = State::Closed;
	QString m_closeReason;
};

}
