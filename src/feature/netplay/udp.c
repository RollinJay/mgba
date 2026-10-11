/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/feature/netplay.h>

#include <mgba-util/socket.h>

#include <gekkonet.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/* A GekkoNet network adapter on one UDP socket (IPv4). Addresses are "a.b.c.d:port"
 * strings. For tests it can delay outgoing packets by a fixed latency plus random jitter
 * and drop a fraction of them. GekkoNet's adapter callbacks carry no context, so there is
 * one adapter per process. */

#define MAX_PACKET 2048
#define MAX_QUEUED 4096

struct _queued {
	int64_t due;
	struct sockaddr_in to;
	int length;
	char data[MAX_PACKET];
};

static struct {
	Socket socket;
	bool open;
	int latencyMs;
	int jitterMs;
	double loss;
	uint32_t rng;
	struct _queued* queue;   /* ring of delayed packets, in due order per send call */
	int head;
	int count;
	GekkoNetResult** results;
	int resultCap;
} _udp = { .socket = INVALID_SOCKET };

static int64_t _nowMs(void) {
#ifdef _WIN32
	return (int64_t) GetTickCount64();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static uint32_t _rand(void) {
	uint32_t x = _udp.rng;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	_udp.rng = x;
	return x;
}

static bool _parseAddress(const void* data, unsigned size, struct sockaddr_in* out) {
	char text[64];
	if (size >= sizeof(text)) {
		return false;
	}
	memcpy(text, data, size);
	text[size] = '\0';
	char* colon = strrchr(text, ':');
	if (!colon) {
		return false;
	}
	*colon = '\0';
	memset(out, 0, sizeof(*out));
	out->sin_family = AF_INET;
	out->sin_port = htons((uint16_t) atoi(colon + 1));
	return inet_pton(AF_INET, text, &out->sin_addr) == 1;
}

static void _sendNow(const struct sockaddr_in* to, const char* data, int length) {
	sendto(_udp.socket, data, length, 0, (const struct sockaddr*) to, sizeof(*to));
}

/* Sends every delayed packet whose time has come. Jitter can reorder packets, as on a
 * real network, because each packet's due time is checked independently. */
static void _flush(void) {
	if (!_udp.count) {
		return;
	}
	int64_t now = _nowMs();
	int i;
	int kept = 0;
	for (i = 0; i < _udp.count; ++i) {
		struct _queued* q = &_udp.queue[(_udp.head + i) % MAX_QUEUED];
		if (q->due <= now) {
			_sendNow(&q->to, q->data, q->length);
		} else {
			if (kept != i) {
				_udp.queue[(_udp.head + kept) % MAX_QUEUED] = *q;
			}
			++kept;
		}
	}
	_udp.count = kept;
}

static void _send(GekkoNetAddress* addr, const char* data, int length) {
	struct sockaddr_in to;
	if (!_udp.open || length <= 0 || length > MAX_PACKET || !_parseAddress(addr->data, addr->size, &to)) {
		return;
	}
	if (_udp.loss > 0 && (_rand() % 1000000) < (uint32_t) (_udp.loss * 1000000)) {
		return;
	}
	if (!_udp.latencyMs && !_udp.jitterMs) {
		_sendNow(&to, data, length);
		return;
	}
	if (_udp.count >= MAX_QUEUED) {
		return;   /* a full queue behaves like a congested link */
	}
	struct _queued* q = &_udp.queue[(_udp.head + _udp.count) % MAX_QUEUED];
	q->due = _nowMs() + _udp.latencyMs + (_udp.jitterMs ? (int) (_rand() % (uint32_t) (_udp.jitterMs + 1)) : 0);
	q->to = to;
	q->length = length;
	memcpy(q->data, data, length);
	++_udp.count;
	_flush();
}

static GekkoNetResult** _receive(int* length) {
	_flush();
	int n = 0;
	char buffer[MAX_PACKET];
	while (_udp.open) {
		struct sockaddr_in from;
		socklen_t fromLen = sizeof(from);
		ssize_t len = recvfrom(_udp.socket, buffer, sizeof(buffer), 0, (struct sockaddr*) &from, &fromLen);
		if (len < 0) {
			if (SocketWouldBlock()) {
				break;
			}
			continue;   /* e.g. ICMP port unreachable reported on Windows */
		}
		if (n == _udp.resultCap) {
			_udp.resultCap = _udp.resultCap ? _udp.resultCap * 2 : 64;
			_udp.results = realloc(_udp.results, sizeof(*_udp.results) * _udp.resultCap);
		}
		char ip[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
		char text[64];
		int textLen = snprintf(text, sizeof(text), "%s:%u", ip, ntohs(from.sin_port));
		GekkoNetResult* res = malloc(sizeof(*res));
		res->addr.size = textLen;
		res->addr.data = malloc(textLen);
		memcpy(res->addr.data, text, textLen);
		res->data_len = (unsigned) len;
		res->data = malloc(len ? len : 1);
		memcpy(res->data, buffer, len);
		_udp.results[n++] = res;
	}
	*length = n;
	return _udp.results;
}

static void _free(void* ptr) {
	free(ptr);
}

static GekkoNetAdapter _adapter = { _send, _receive, _free };

GekkoNetAdapter* mNetplayUDPAdapterCreate(int port, int latencyMs, int jitterMs, double loss, uint32_t seed, char* err, size_t errlen) {
	mNetplayUDPAdapterDestroy();
	SocketSubsystemInit();
	Socket s = socket(AF_INET, SOCK_DGRAM, 0);
	if (SOCKET_FAILED(s)) {
		snprintf(err, errlen, "cannot create a UDP socket");
		return NULL;
	}
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t) port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(s, (struct sockaddr*) &addr, sizeof(addr)) != 0) {
		snprintf(err, errlen, "cannot bind UDP port %d", port);
		SocketClose(s);
		return NULL;
	}
	SocketSetBlocking(s, false);
	_udp.socket = s;
	_udp.open = true;
	_udp.latencyMs = latencyMs;
	_udp.jitterMs = jitterMs;
	_udp.loss = loss;
	_udp.rng = seed ? seed : 1;
	_udp.queue = calloc(MAX_QUEUED, sizeof(*_udp.queue));
	_udp.head = 0;
	_udp.count = 0;
	return &_adapter;
}

void mNetplayUDPAdapterDestroy(void) {
	if (_udp.open) {
		SocketClose(_udp.socket);
		_udp.open = false;
		_udp.socket = INVALID_SOCKET;
	}
	free(_udp.queue);
	_udp.queue = NULL;
	free(_udp.results);
	_udp.results = NULL;
	_udp.resultCap = 0;
	_udp.count = 0;
}
