/* SPDX-License-Identifier: MIT
 * Part of netplay-online (https://github.com/RollinJay/Nakama-Matchmaker). */
#pragma once

#include <QString>
#include <QtGlobal>

namespace NetplayOnline {

/* An IPv4 address and port, in host byte order. */
struct Address {
	quint32 ip = 0;
	quint16 port = 0;

	bool isValid() const { return ip != 0 && port != 0; }
	QString toString() const;
	static bool parse(const QString& host, int port, Address* out);   // dotted IPv4 only
	bool operator==(const Address& other) const { return ip == other.ip && port == other.port; }
	bool operator!=(const Address& other) const { return !(*this == other); }
};

/* A non-blocking IPv4 UDP socket that any one thread may use without an event loop:
 * the netplay thread polls it every frame. */
class UdpSocket {
public:
	UdpSocket();
	~UdpSocket();
	UdpSocket(const UdpSocket&) = delete;
	UdpSocket& operator=(const UdpSocket&) = delete;

	/* Binds the port, or an ephemeral port when that one is taken or 0. */
	bool open(quint16 port, QString* error);
	void close();
	bool isOpen() const;
	quint16 localPort() const;

	bool sendTo(const Address& to, const char* data, int length);
	/* Returns the datagram's length, or -1 when nothing is waiting. */
	int receive(char* buffer, int capacity, Address* from);
	/* Waits up to timeoutMs for a datagram to arrive. */
	bool wait(int timeoutMs);

private:
	qintptr m_fd;
	quint16 m_port = 0;
};

}
