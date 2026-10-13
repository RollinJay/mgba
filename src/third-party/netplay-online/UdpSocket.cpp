/* SPDX-License-Identifier: MIT
 * Part of netplay-online (https://github.com/RollinJay/Nakama-Matchmaker). */
#include "UdpSocket.h"

#include <QStringList>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define CLOSE_SOCKET closesocket
#define BAD_SOCKET ((qintptr) INVALID_SOCKET)
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSE_SOCKET ::close
#define BAD_SOCKET ((qintptr) -1)
#endif

#include <cstring>

using namespace NetplayOnline;

namespace {

#ifdef _WIN32
bool startWinsock() {
	static bool started = false;
	if (!started) {
		WSADATA data;
		started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
	}
	return started;
}
#endif

sockaddr_in toSockaddr(const Address& a) {
	sockaddr_in s;
	memset(&s, 0, sizeof(s));
	s.sin_family = AF_INET;
	s.sin_addr.s_addr = htonl(a.ip);
	s.sin_port = htons(a.port);
	return s;
}

}

QString Address::toString() const {
	return QStringLiteral("%1.%2.%3.%4:%5").arg((ip >> 24) & 0xFF).arg((ip >> 16) & 0xFF).arg((ip >> 8) & 0xFF).arg(ip & 0xFF).arg(port);
}

bool Address::parse(const QString& host, int port, Address* out) {
	QStringList parts = host.split(QLatin1Char('.'));
	if (parts.size() != 4 || port <= 0 || port > 65535) {
		return false;
	}
	quint32 ip = 0;
	for (const QString& part : parts) {
		bool ok = false;
		int value = part.toInt(&ok);
		if (!ok || value < 0 || value > 255) {
			return false;
		}
		ip = (ip << 8) | quint32(value);
	}
	out->ip = ip;
	out->port = quint16(port);
	return true;
}

UdpSocket::UdpSocket()
	: m_fd(BAD_SOCKET) {
}

UdpSocket::~UdpSocket() {
	close();
}

bool UdpSocket::open(quint16 port, QString* error) {
	close();
#ifdef _WIN32
	if (!startWinsock()) {
		*error = QStringLiteral("cannot start Winsock");
		return false;
	}
#endif
	for (int attempt = 0; attempt < 2; ++attempt) {
		quint16 bindPort = attempt == 0 ? port : 0;
		if (attempt == 1 && port == 0) {
			break;
		}
		qintptr fd = qintptr(::socket(AF_INET, SOCK_DGRAM, 0));
		if (fd == BAD_SOCKET) {
			*error = QStringLiteral("cannot create a UDP socket");
			return false;
		}
		sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		addr.sin_port = htons(bindPort);
		if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
			CLOSE_SOCKET(fd);
			continue;   // the port is taken: try an ephemeral one
		}
#ifdef _WIN32
		u_long nonblocking = 1;
		ioctlsocket(fd, FIONBIO, &nonblocking);
#else
		fcntl(int(fd), F_SETFL, fcntl(int(fd), F_GETFL, 0) | O_NONBLOCK);
#endif
		sockaddr_in bound;
		socklen_t length = sizeof(bound);
		getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &length);
		m_fd = fd;
		m_port = ntohs(bound.sin_port);
		return true;
	}
	*error = QStringLiteral("cannot bind UDP port %1").arg(port);
	return false;
}

void UdpSocket::close() {
	if (m_fd != BAD_SOCKET) {
		CLOSE_SOCKET(m_fd);
		m_fd = BAD_SOCKET;
	}
	m_port = 0;
}

bool UdpSocket::isOpen() const {
	return m_fd != BAD_SOCKET;
}

quint16 UdpSocket::localPort() const {
	return m_port;
}

bool UdpSocket::sendTo(const Address& to, const char* data, int length) {
	if (m_fd == BAD_SOCKET || !to.isValid()) {
		return false;
	}
	sockaddr_in addr = toSockaddr(to);
	return ::sendto(m_fd, data, length, 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == length;
}

int UdpSocket::receive(char* buffer, int capacity, Address* from) {
	if (m_fd == BAD_SOCKET) {
		return -1;
	}
	while (true) {
		sockaddr_in addr;
		socklen_t length = sizeof(addr);
		int n = int(::recvfrom(m_fd, buffer, capacity, 0, reinterpret_cast<sockaddr*>(&addr), &length));
		if (n < 0) {
#ifdef _WIN32
			if (WSAGetLastError() == WSAECONNRESET) {
				continue;   // an earlier send hit a closed port
			}
#else
			if (errno == EINTR) {
				continue;
			}
#endif
			return -1;
		}
		from->ip = ntohl(addr.sin_addr.s_addr);
		from->port = ntohs(addr.sin_port);
		return n;
	}
}

bool UdpSocket::wait(int timeoutMs) {
	if (m_fd == BAD_SOCKET) {
		return false;
	}
#ifdef _WIN32
	fd_set set;
	FD_ZERO(&set);
	FD_SET(SOCKET(m_fd), &set);
	timeval tv { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
	return select(0, &set, nullptr, nullptr, &tv) > 0;
#else
	pollfd p { int(m_fd), POLLIN, 0 };
	return ::poll(&p, 1, timeoutMs) > 0;
#endif
}
