/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QMutex>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QVector>

#include <atomic>
#include <functional>
#include <vector>

#include <mgba-util/audio-buffer.h>

struct GBALinkMachine;
struct mLogger;
struct mNetplayProfile;
struct mNetplaySession;

namespace NetplayOnline {
class PeerLinks;
}

namespace QGBA {

/* Runs a netplay profile's link machine under GekkoNet on its own thread, at the GBA's
 * frame rate: online with every other player over PeerLinks, or with every player on this
 * computer. It keeps the latest pictures of the GBAs it draws and the sound of the local
 * player's GBA, resampled to 48 kHz, for the window; frames that GekkoNet re-simulates
 * are neither shown nor heard. The thread owns the PeerLinks while it runs. */
class NetplayRunner : public QThread {
	Q_OBJECT

public:
	enum class Mode { Local, StressTest, Online };

	struct Options {
		QString profilePath;
		Mode mode = Mode::Local;
		int localPlayer = 0;                 // online: this computer's player (from 0)
		QStringList playerIds;               // online: every player's id in order (the local one included)
		NetplayOnline::PeerLinks* links = nullptr;
		QStringList stunServers;
		int port = 0;
		int inputDelay = -1;                 // frames; -1 picks one from the round trip
		bool drawAll = false;                // draw every GBA, not only the local player's
		bool playIntro = false;              // play the profile's scripted intro first (tests)
		int maxFrames = 0;                   // stop after this many session frames (tests)
		std::function<uint16_t(int player, int frame)> bot;   // scripted keys (tests)
		QString tracePath;                   // tests: per-frame checksum and keys
		mLogger* logger = nullptr;           // receives the GBAs' log messages; outlives the thread
	};

	struct Stats {
		int frame = -1;
		int rollbacks = 0;
		int resimFrames = 0;
		int lastRollback = 0;     // re-simulated frames in the latest rollback
		int maxRollback = 0;
		int desyncs = 0;
		int inputDelay = 0;
		float framesAhead = 0;
		QVector<int> roundTripMs; // per player, -1 for the local one or unknown
		uint32_t checksum = 0;
	};

	explicit NetplayRunner(const Options&, QObject* parent = nullptr);
	~NetplayRunner() override;

	void setKeys(uint16_t keys) { m_keys = keys; }
	void requestStop() { m_stop = true; }
	/* Draws every GBA (true) or only the local player's; applied from the next frame. */
	void setDrawAll(bool all) { m_drawAllWanted = all; }

	/* The latest picture of a GBA (videoSize() of that GBA, rows packed, 0xXXBBGGRR), and
	 * whether one exists. */
	bool picture(int gba, std::vector<uint32_t>* out) const;
	/* A GBA's picture size: 240x160, or larger for a ROM that declares widescreen margins
	 * (doc/widescreen.md); 0x0 before the session opens the GBAs. */
	void videoSize(int gba, int* width, int* height) const;
	int gbas() const { return m_gbas; }
	int localGba() const { return m_localGba; }
	bool drawsAll() const { return m_drawAll; }
	Stats stats() const;
	/* Stereo samples at 48 kHz; returns how many were copied. */
	size_t readAudio(int16_t* samples, size_t count);
	size_t audioAvailable() const;

signals:
	void linked(bool ok, const QString& error);
	void sessionStarted();
	void desyncDetected(int frame);
	void playerDisconnected(int player);
	void finished(const QString& error);

protected:
	void run() override;

private:
	static void onFrame(mNetplaySession*, bool shown, void* context);
	bool waitForLinks(QString* error);
	QString runSession();

	Options m_options;
	std::atomic<uint16_t> m_keys { 0 };
	std::atomic<bool> m_stop { false };
	std::atomic<bool> m_drawAllWanted { false };
	std::atomic<bool> m_drawAll { false };
	std::atomic<int> m_gbas { 0 };
	std::atomic<int> m_localGba { 0 };
	static constexpr int kMaxGBAs = 4;
	std::atomic<int> m_videoWidth[kMaxGBAs] {};
	std::atomic<int> m_videoHeight[kMaxGBAs] {};
	GBALinkMachine* m_machine = nullptr;

	mutable QMutex m_lock;   // guards the pictures, the stats and the audio buffer
	std::vector<std::vector<uint32_t>> m_pictures;
	Stats m_stats;
	mAudioBuffer m_audio;
	int m_pendingResim = 0;
};

}
