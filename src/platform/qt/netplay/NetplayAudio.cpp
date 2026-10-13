/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "NetplayAudio.h"

#include "NetplayRunner.h"

#include <cstring>

using namespace QGBA;

NetplayAudio::NetplayAudio(QObject* parent)
	: QIODevice(parent) {
	open(QIODevice::ReadOnly);
}

void NetplayAudio::setRunner(NetplayRunner* runner) {
	m_runner = runner;
}

qint64 NetplayAudio::bytesAvailable() const {
	qint64 available = m_runner ? qint64(m_runner->audioAvailable()) * 4 : 0;
	return available + QIODevice::bytesAvailable();
}

qint64 NetplayAudio::readData(char* data, qint64 maxSize) {
	qint64 frames = maxSize / 4;
	if (frames <= 0) {
		return 0;
	}
	if (m_runner && m_runner->audioAvailable() > 6000) {
		// more than 125 ms waiting: drop the oldest part to keep the delay short
		int16_t scratch[2 * 1024];
		size_t excess = m_runner->audioAvailable() - 3000;
		while (excess > 0) {
			size_t chunk = excess < 1024 ? excess : 1024;
			size_t got = m_runner->readAudio(scratch, chunk);
			if (!got) {
				break;
			}
			excess -= got;
		}
	}
	size_t got = m_runner ? m_runner->readAudio(reinterpret_cast<int16_t*>(data), size_t(frames)) : 0;
	if (qint64(got) < frames) {
		memset(data + got * 4, 0, size_t(frames - qint64(got)) * 4);
	}
	return frames * 4;
}
