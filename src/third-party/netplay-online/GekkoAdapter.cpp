/* SPDX-License-Identifier: MIT
 * Part of netplay-online (https://github.com/RollinJay/Nakama-Matchmaker). */
#include "GekkoAdapter.h"

#include "PeerLinks.h"

#include <gekkonet.h>

#include <cstdlib>
#include <cstring>
#include <vector>

using namespace NetplayOnline;

namespace {

PeerLinks* s_links = nullptr;
std::vector<GekkoNetResult*> s_results;
QVector<QPair<int, QByteArray>> s_packets;

void sendData(GekkoNetAddress* addr, const char* data, int length) {
	if (!s_links || !addr || length <= 0) {
		return;
	}
	QString id = QString::fromUtf8(static_cast<const char*>(addr->data), int(addr->size));
	s_links->sendGame(s_links->peerIndex(id), data, length);
}

GekkoNetResult** receiveData(int* length) {
	s_results.clear();
	if (s_links) {
		s_links->poll();
		s_packets.clear();
		s_links->takeGamePackets(&s_packets);
		for (const auto& packet : s_packets) {
			QByteArray id = s_links->peerId(packet.first).toUtf8();
			GekkoNetResult* result = static_cast<GekkoNetResult*>(malloc(sizeof(GekkoNetResult)));
			result->addr.size = unsigned(id.size());
			result->addr.data = malloc(id.size() ? id.size() : 1);
			memcpy(result->addr.data, id.constData(), id.size());
			result->data_len = unsigned(packet.second.size());
			result->data = malloc(packet.second.size() ? packet.second.size() : 1);
			memcpy(result->data, packet.second.constData(), packet.second.size());
			s_results.push_back(result);
		}
	}
	*length = int(s_results.size());
	return s_results.data();
}

void freeData(void* pointer) {
	free(pointer);
}

GekkoNetAdapter s_adapter = { sendData, receiveData, freeData };

}

GekkoNetAdapter* NetplayOnline::gekkoAdapter(PeerLinks* links) {
	s_links = links;
	return &s_adapter;
}

void NetplayOnline::gekkoAdapterDetach() {
	s_links = nullptr;
}
