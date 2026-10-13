/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "NetplayRunner.h"

#include <GekkoAdapter.h>
#include <PeerLinks.h>

#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/feature/netplay.h>
#include <mgba-util/audio-resampler.h>

#include <gekkonet.h>

#include <QElapsedTimer>
#include <QFile>

#include <memory>

using namespace QGBA;

namespace {

const qint64 FRAME_NS = qint64(1000000000) * GBA_LINK_MACHINE_FRAME_CYCLES / 16777216;
const unsigned AUDIO_RATE = 48000;

struct AudioState {
	mAudioResampler resampler;
	mAudioBuffer resampled;
	mCore* core = nullptr;
};

}

NetplayRunner::NetplayRunner(const Options& options, QObject* parent)
	: QThread(parent)
	, m_options(options) {
	m_drawAllWanted = options.drawAll;
	m_drawAll = options.drawAll;
	mAudioBufferInit(&m_audio, 0x4000, 2);
}

NetplayRunner::~NetplayRunner() {
	m_stop = true;
	wait();
	mAudioBufferDeinit(&m_audio);
}

void NetplayRunner::videoSize(int gba, int* width, int* height) const {
	if (gba < 0 || gba >= kMaxGBAs) {
		*width = 0;
		*height = 0;
		return;
	}
	*width = m_videoWidth[gba];
	*height = m_videoHeight[gba];
}

bool NetplayRunner::picture(int gba, std::vector<uint32_t>* out) const {
	QMutexLocker locker(&m_lock);
	if (gba < 0 || gba >= int(m_pictures.size()) || m_pictures[gba].empty()) {
		return false;
	}
	*out = m_pictures[gba];
	return true;
}

NetplayRunner::Stats NetplayRunner::stats() const {
	QMutexLocker locker(&m_lock);
	return m_stats;
}

size_t NetplayRunner::readAudio(int16_t* samples, size_t count) {
	QMutexLocker locker(&m_lock);
	return mAudioBufferRead(&m_audio, samples, count);
}

size_t NetplayRunner::audioAvailable() const {
	QMutexLocker locker(&m_lock);
	return mAudioBufferAvailable(&m_audio);
}

void NetplayRunner::run() {
	if (m_options.logger) {
		mLogSetThreadLogger(m_options.logger);
	}
	QString error = runSession();
	mLogSetThreadLogger(nullptr);
	emit finished(error);
}

/* Called after every frame the session advances. */
void NetplayRunner::onFrame(mNetplaySession* s, bool shown, void* context) {
	NetplayRunner* self = static_cast<NetplayRunner*>(context);
	mCore* core = GBALinkMachineCore(s->machine, self->m_localGba);
	if (!shown) {
		mAudioBufferClear(core->getAudioBuffer(core));
		++self->m_pendingResim;
		return;
	}
	QMutexLocker locker(&self->m_lock);
	for (int i = 0; i < self->m_gbas; ++i) {
		if (!self->m_drawAll && i != self->m_localGba) {
			continue;
		}
		const mColor* fb = GBALinkMachineFramebuffer(s->machine, i);
		size_t pixels = size_t(self->m_videoWidth[i]) * size_t(self->m_videoHeight[i]);
		std::vector<uint32_t>& picture = self->m_pictures[i];
		picture.resize(pixels);
		memcpy(picture.data(), fb, pixels * sizeof(uint32_t));
	}
}

bool NetplayRunner::waitForLinks(QString* error) {
	NetplayOnline::PeerLinks* links = m_options.links;
	if (!links->start(quint16(m_options.port), m_options.stunServers, error)) {
		return false;
	}
	while (!m_stop) {
		links->poll();
		if (links->allConnected()) {
			return true;
		}
		if (links->failed()) {
			*error = links->error();
			return false;
		}
		QThread::msleep(5);
	}
	*error = QStringLiteral("stopped");
	return false;
}

QString NetplayRunner::runSession() {
	std::unique_ptr<mNetplayProfile> profile(new mNetplayProfile);
	char err[512];
	QByteArray path = m_options.profilePath.toLocal8Bit();
	if (!mNetplayProfileLoad(profile.get(), path.constData(), err, sizeof(err))) {
		return QString::fromLocal8Bit(err);
	}
	bool online = m_options.mode == Mode::Online;
	int localPlayer = online ? m_options.localPlayer : 0;
	if (localPlayer < 0 || localPlayer >= profile->players) {
		return QStringLiteral("the profile has %1 players, not %2").arg(profile->players).arg(localPlayer + 1);
	}
	if (online && (!m_options.links || m_options.playerIds.size() != profile->players)) {
		return QStringLiteral("the match has %1 players, the profile %2").arg(m_options.playerIds.size()).arg(profile->players);
	}
	GBALinkMachine* machine = mNetplayProfileOpen(profile.get(), err, sizeof(err));
	if (!machine) {
		return QString::fromLocal8Bit(err);
	}
	m_machine = machine;
	for (int i = 0; i < GBALinkMachineGBAs(machine) && i < kMaxGBAs; ++i) {
		unsigned width, height;
		GBALinkMachineVideoSize(machine, i, &width, &height);
		m_videoWidth[i] = int(width);
		m_videoHeight[i] = int(height);
	}
	m_gbas = GBALinkMachineGBAs(machine);   // after the sizes: the window reads them in this order
	m_localGba = profile->player[localPlayer].gba;
	{
		QMutexLocker locker(&m_lock);
		m_pictures.assign(m_gbas, std::vector<uint32_t>());
		m_stats.roundTripMs = QVector<int>(profile->players, -1);
	}
	GBALinkMachineSetDrawMask(machine, m_drawAll ? (1u << m_gbas) - 1 : 1u << m_localGba);

	mCore* audioCore = GBALinkMachineCore(machine, m_localGba);
	AudioState audio;
	mAudioBufferInit(&audio.resampled, 0x4000, 2);
	mAudioResamplerInit(&audio.resampler, mINTERPOLATOR_SINC);
	mAudioResamplerSetSource(&audio.resampler, audioCore->getAudioBuffer(audioCore), audioCore->audioSampleRate(audioCore), true);
	mAudioResamplerSetDestination(&audio.resampler, &audio.resampled, AUDIO_RATE);

	QString result;
	GekkoSession* gekko = nullptr;
	mNetplaySession session;
	bool sessionInit = false;

	if (m_options.playIntro) {
		int introFrames = mNetplayProfileIntroFrames(profile.get());
		GBALinkMachineSetRender(machine, false);
		for (int f = 0; f < introFrames && !m_stop; ++f) {
			uint16_t keys[NETPLAY_MAX_PLAYERS];
			mNetplayProfileIntroKeys(profile.get(), f, keys);
			mNetplayRunFrame(machine, profile.get(), keys);
		}
		GBALinkMachineSetRender(machine, true);
		for (int i = 0; i < m_gbas; ++i) {
			mCore* core = GBALinkMachineCore(machine, i);
			mAudioBufferClear(core->getAudioBuffer(core));
		}
	}

	int inputDelay = m_options.inputDelay;
	if (online) {
		QString linkError;
		bool ok = waitForLinks(&linkError);
		emit linked(ok, linkError);
		if (!ok) {
			m_options.links->close();
			mAudioResamplerDeinit(&audio.resampler);
			mAudioBufferDeinit(&audio.resampled);
			GBALinkMachineClose(machine);
			m_machine = nullptr;
			return linkError;
		}
		if (inputDelay < 0) {
			// a second and a half of pings, then a delay that covers half the slowest round trip
			QElapsedTimer wait;
			wait.start();
			int worst = -1;
			while (!m_stop && wait.elapsed() < 1500) {
				m_options.links->poll();
				QThread::msleep(5);
			}
			for (int i = 0; i < m_options.links->peers(); ++i) {
				worst = qMax(worst, m_options.links->roundTripMs(i));
			}
			int oneWay = worst < 0 ? 50 : worst / 2;
			inputDelay = qBound(1, int((oneWay + 8) * 1000000LL / FRAME_NS), 6);
		}
	}
	if (inputDelay < 0) {
		inputDelay = online ? 2 : 0;
	}

	{
		GekkoConfig config {};
		config.num_players = (unsigned char) profile->players;
		config.max_spectators = 0;
		config.max_input_prediction_window = 8;
		config.input_size = NETPLAY_INPUT_SIZE;
		config.state_size = (unsigned) GBALinkMachineStateSize(machine);
		config.desync_detection = true;
		if (m_options.mode == Mode::StressTest) {
			gekko_create(&gekko, GekkoStressSession);
			config.check_distance = 7;
		} else {
			gekko_create(&gekko, GekkoGameSession);
		}
		gekko_start(gekko, &config);
		if (online) {
			gekko_net_adapter_set(gekko, NetplayOnline::gekkoAdapter(m_options.links));
			gekko_set_disconnect_timeout(gekko, 10000);
		}
	}
	std::vector<int> handles(profile->players, -1);
	std::vector<QByteArray> addresses(profile->players);
	for (int p = 0; p < profile->players; ++p) {
		if (!online || p == localPlayer) {
			handles[p] = gekko_add_actor(gekko, GekkoLocalPlayer, nullptr);
			if (online) {
				gekko_set_local_delay(gekko, handles[p], (unsigned char) inputDelay);
			}
		} else {
			addresses[p] = m_options.playerIds[p].toUtf8();
			GekkoNetAddress address { addresses[p].data(), unsigned(addresses[p].size()) };
			handles[p] = gekko_add_actor(gekko, GekkoRemotePlayer, &address);
		}
	}
	mNetplaySessionInit(&session, machine, profile.get(), gekko);
	session.frameCallback = &NetplayRunner::onFrame;
	session.frameContext = this;
	sessionInit = true;
	if (!m_options.tracePath.isEmpty()) {
		mNetplaySessionEnableTrace(&session, (m_options.maxFrames ? m_options.maxFrames : 108000) + 64);
	}
	{
		QMutexLocker locker(&m_lock);
		m_stats.inputDelay = inputDelay;
	}

	{
		QElapsedTimer clock;
		clock.start();
		qint64 next = clock.nsecsElapsed();
		int n = 0;
		bool started = !online;
		if (started) {
			emit sessionStarted();
		}
		while (!m_stop && (m_options.maxFrames == 0 || session.stats.frame + 1 < m_options.maxFrames)) {
			if (online) {
				gekko_network_poll(gekko);
			}
			int eventCount = 0;
			GekkoSessionEvent** events = gekko_session_events(gekko, &eventCount);
			for (int i = 0; i < eventCount; ++i) {
				switch (events[i]->type) {
				case GekkoDesyncDetected:
					++session.stats.desyncs;
					emit desyncDetected(events[i]->data.desynced.frame);
					break;
				case GekkoPlayerDisconnected:
					emit playerDisconnected(events[i]->data.disconnected.handle);
					result = QStringLiteral("player %1 disconnected").arg(events[i]->data.disconnected.handle + 1);
					m_stop = true;
					break;
				case GekkoSessionStarted:
					if (!started) {
						started = true;
						emit sessionStarted();
					}
					break;
				default:
					break;
				}
			}
			if (m_stop) {
				break;
			}

			qint64 now = clock.nsecsElapsed();
			if (now < next) {
				QThread::usleep((unsigned long) ((next - now) / 1000));
			}
			float ahead = online ? gekko_frames_ahead(gekko) : 0.0f;
			next += ahead >= 1.0f ? FRAME_NS + FRAME_NS / 50 : FRAME_NS;
			if (clock.nsecsElapsed() - next > 4 * FRAME_NS) {
				next = clock.nsecsElapsed();   // do not try to catch up after a stall
			}

			if (m_drawAllWanted != m_drawAll) {
				m_drawAll = bool(m_drawAllWanted);
				GBALinkMachineSetDrawMask(machine, m_drawAll ? (1u << m_gbas) - 1 : 1u << m_localGba);
			}
			for (int p = 0; p < profile->players; ++p) {
				if (online && p != localPlayer) {
					continue;
				}
				uint16_t keys = 0;
				if (m_options.bot) {
					keys = m_options.bot(p, n);
				} else if (p == localPlayer) {
					keys = m_keys;
				}
				uint8_t input[NETPLAY_INPUT_SIZE];
				mNetplayEncodeKeys(keys, input);
				gekko_add_local_input(gekko, handles[p], input);
			}
			++n;

			int count = 0;
			GekkoGameEvent** gameEvents = gekko_update_session(gekko, &count);
			m_pendingResim = 0;
			mNetplaySessionHandleEvents(&session, gameEvents, count);
			mAudioResamplerProcess(&audio.resampler);
			int16_t samples[2 * 2048];
			size_t got;
			{
				QMutexLocker locker(&m_lock);
				while ((got = mAudioBufferRead(&audio.resampled, samples, 2048)) > 0) {
					mAudioBufferWrite(&m_audio, samples, got);
				}
				m_stats.frame = session.stats.frame;
				m_stats.rollbacks = session.stats.rollbacks;
				m_stats.resimFrames = session.stats.resimFrames;
				if (m_pendingResim) {
					m_stats.lastRollback = m_pendingResim;
					m_stats.maxRollback = qMax(m_stats.maxRollback, m_pendingResim);
				}
				m_stats.desyncs = session.stats.desyncs;
				m_stats.framesAhead = ahead;
				m_stats.checksum = session.checksum;
				if (online) {
					for (int p = 0; p < profile->players; ++p) {
						int peer = p == localPlayer ? -1 : m_options.links->peerIndex(m_options.playerIds[p]);
						m_stats.roundTripMs[p] = peer < 0 ? -1 : m_options.links->roundTripMs(peer);
					}
				}
			}
			for (int i = 0; i < m_gbas; ++i) {
				if (i != m_localGba) {
					mCore* core = GBALinkMachineCore(machine, i);
					mAudioBufferClear(core->getAudioBuffer(core));
				}
			}
		}
	}
	if (online) {
		// keep answering for a moment so the other players can confirm the last frames
		QElapsedTimer linger;
		linger.start();
		while (linger.elapsed() < 500) {
			gekko_network_poll(gekko);
			QThread::msleep(5);
		}
	}
	if (!m_options.tracePath.isEmpty() && sessionInit) {
		QFile file(m_options.tracePath);
		if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			for (int i = 0; i <= session.stats.frame && i < session.traceLength; ++i) {
				QByteArray line = QByteArray::number(i) + ' ' + QByteArray::number(session.traceChecksum[i], 16).rightJustified(8, '0');
				for (int p = 0; p < profile->players; ++p) {
					line += ' ' + QByteArray::number(session.traceKeys[i * NETPLAY_MAX_PLAYERS + p], 16).rightJustified(3, '0');
				}
				file.write(line + '\n');
			}
		}
	}

	if (online) {
		NetplayOnline::gekkoAdapterDetach();
		m_options.links->close();
	}
	if (sessionInit) {
		mNetplaySessionDeinit(&session);
	}
	if (gekko) {
		gekko_destroy(&gekko);
	}
	mAudioResamplerDeinit(&audio.resampler);
	mAudioBufferDeinit(&audio.resampled);
	GBALinkMachineClose(machine);
	m_machine = nullptr;
	return result;
}
