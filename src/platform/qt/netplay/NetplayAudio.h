/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QIODevice>
#include <QPointer>

namespace QGBA {

class NetplayRunner;

/* The sound of the local player's GBA (48 kHz, 16-bit stereo), pulled by Qt Multimedia
 * from the netplay runner. When the runner has nothing, it plays silence rather than
 * stalling, since the session's pace is set by GekkoNet, not by the sound card. */
class NetplayAudio : public QIODevice {
	Q_OBJECT

public:
	explicit NetplayAudio(QObject* parent = nullptr);

	void setRunner(NetplayRunner*);
	bool isSequential() const override { return true; }
	qint64 bytesAvailable() const override;

protected:
	qint64 readData(char* data, qint64 maxSize) override;
	qint64 writeData(const char*, qint64) override { return -1; }

private:
	QPointer<NetplayRunner> m_runner;
};

}
