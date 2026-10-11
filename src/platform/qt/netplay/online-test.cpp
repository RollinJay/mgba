/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/* mgba-netplay-online-test: one player of an online match, headless. It finds a match
 * through a Nakama server (or joins the one that a launcher found, from NETPLAY_CONNECT),
 * opens the direct connections and plays the profile under GekkoNet with scripted keys,
 * like `mgba-netplay-test -m peer` but over the whole online path.
 * tools/netplay/run_online_test.sh runs several of them against a server. */
#include "NetplayRunner.h"

#include <MatchSession.h>
#include <PeerLinks.h>

#include <mgba/core/log.h>
#include <mgba/feature/netplay.h>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>

#include <cstdio>
#include <memory>

using namespace QGBA;
using namespace NetplayOnline;

namespace {

void nolog(struct mLogger*, int, enum mLogLevel, const char*, va_list) {
}

struct mLogger s_logger = { nolog, nullptr };

uint32_t hash(uint32_t x) {
	x ^= x >> 16;
	x *= 0x7FEB352D;
	x ^= x >> 15;
	x *= 0x846CA68B;
	x ^= x >> 16;
	return x;
}

/* the same scripted keys as mgba-netplay-test */
uint16_t bot(uint32_t seed, int player, int n) {
	static const uint16_t moves[] = { 0, 0x10, 0x20, 0x80, 0x40, 0x50, 0xA0 };
	static const uint16_t buttons[] = { 0, 0, 0x001, 0x002, 0x200, 0x100 };
	uint32_t h = hash(seed * 7919 + player * 104729 + uint32_t(n / 6) * 2654435761u);
	return moves[h % 7] | buttons[(h >> 8) % 6];
}

}

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	QCommandLineParser parser;
	parser.addHelpOption();
	parser.addOption({ "profile", "netplay profile", "file" });
	parser.addOption({ "host", "Nakama server host", "host", "127.0.0.1" });
	parser.addOption({ "port", "Nakama server port", "port", "7350" });
	parser.addOption({ "key", "Nakama server key", "key", "defaultkey" });
	parser.addOption({ "device", "device id (one account per id)", "id" });
	parser.addOption({ "name", "player name", "name", "tester" });
	parser.addOption({ "frames", "session frames", "n", "1800" });
	parser.addOption({ "seed", "scripted key seed", "n", "1" });
	parser.addOption({ "delay", "input delay in frames (default: from the round trip)", "n", "-1" });
	parser.addOption({ "trace", "write each frame's checksum and keys", "file" });
	parser.addOption({ "stun", "STUN server host:port", "server" });
	parser.addOption({ "timeout", "seconds before giving up", "s", "180" });
	parser.addOption({ "identity", "print the profile's identity and exit" });
	parser.process(app);
	mLogSetDefaultLogger(&s_logger);

	ConnectInfo launch;
	QString launchError;
	// --identity leaves a launcher's connection file alone
	bool fromLauncher = !parser.isSet("identity") && ConnectInfo::fromEnvironment(&launch, &launchError);
	if (!fromLauncher && !launchError.isEmpty()) {
		fprintf(stderr, "%s\n", qPrintable(launchError));
		return 1;
	}
	QString profilePath = fromLauncher ? launch.profile : parser.value("profile");
	static mNetplayProfile profile;
	char err[512];
	if (!mNetplayProfileLoad(&profile, profilePath.toLocal8Bit().constData(), err, sizeof(err))) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	char identity[41];
	if (!mNetplayProfileIdentity(&profile, identity, err, sizeof(err))) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	if (parser.isSet("identity")) {
		printf("%s\n", identity);
		return 0;
	}
	if (fromLauncher && !launch.identity.isEmpty() && launch.identity != QString::fromLatin1(identity)) {
		fprintf(stderr, "the launcher matched on another identity: the profile or its files differ\n");
		return 1;
	}
	QString name = fromLauncher ? launch.players.value(launch.self).name : parser.value("name");
	uint32_t seed = parser.value("seed").toUInt();
	int frames = parser.value("frames").toInt();

	MatchSession session;
	std::unique_ptr<NetplayRunner> runner;
	int exitCode = 1;
	QElapsedTimer clock;
	clock.start();

	auto finish = [&](int code) {
		exitCode = code;
		if (runner) {
			runner->requestStop();
			runner->wait();
		}
		session.leave();
		QTimer::singleShot(300, &app, &QCoreApplication::quit);
	};

	QObject::connect(&session, &MatchSession::failed, [&](const QString& error) {
		fprintf(stdout, "%s: FAILED: %s\n", qPrintable(name), qPrintable(error));
		finish(1);
	});
	QObject::connect(&session, &MatchSession::linking, [&]() {
		NetplayRunner::Options options;
		options.profilePath = profilePath;
		options.mode = NetplayRunner::Mode::Online;
		options.localPlayer = session.self();
		for (const MatchPlayer& p : session.players()) {
			options.playerIds.append(p.userId);
		}
		options.links = session.links();
		options.stunServers = session.stunServers();
		options.port = session.port();
		options.inputDelay = fromLauncher && launch.inputDelay >= 0 ? launch.inputDelay : parser.value("delay").toInt();
		options.playIntro = true;
		options.maxFrames = frames;
		options.bot = [seed](int player, int n) { return bot(seed, player + 1, n); };
		options.tracePath = parser.value("trace");
		runner.reset(new NetplayRunner(options));
		QObject::connect(runner.get(), &NetplayRunner::linked, &session, [&](bool ok, const QString& error) {
			session.setLinked(ok, error);
		}, Qt::QueuedConnection);
		QObject::connect(runner.get(), &NetplayRunner::finished, &app, [&](const QString& error) {
			NetplayRunner::Stats st = runner->stats();
			if (!error.isEmpty()) {
				fprintf(stdout, "%s: FAILED: %s\n", qPrintable(name), qPrintable(error));
				finish(1);
				return;
			}
			fprintf(stdout, "player %d mode online profile %s: session %d frames in %.1fs, input delay %d, rollbacks %d, "
			        "re-simulated frames %d, largest rollback %d, desyncs %d, final checksum %08x\n",
			        session.self() + 1, profile.name, st.frame + 1, clock.elapsed() / 1000.0, st.inputDelay, st.rollbacks,
			        st.resimFrames, st.maxRollback, st.desyncs, st.checksum);
			fflush(stdout);
			finish(st.desyncs ? 2 : 0);
		}, Qt::QueuedConnection);
		runner->start();
	});
	QTimer::singleShot(parser.value("timeout").toInt() * 1000, [&]() {
		fprintf(stdout, "%s: TIMEOUT in state %d\n", qPrintable(name), int(session.state()));
		finish(1);
	});

	if (fromLauncher) {
		session.startFromLauncher(launch);
	} else {
		if (!parser.isSet("device")) {
			fprintf(stderr, "--device is required without NETPLAY_CONNECT\n");
			return 1;
		}
		MatchSession::QuickMatch q;
		q.server.host = parser.value("host");
		q.server.port = parser.value("port").toInt();
		q.server.serverKey = parser.value("key");
		q.deviceId = parser.value("device");
		q.name = name;
		q.kind = QStringLiteral("mgba");
		q.identity = QString::fromLatin1(identity);
		q.players = profile.players;
		if (parser.isSet("stun")) {
			q.stunServers.append(parser.value("stun"));
		}
		session.startQuickMatch(q);
	}
	app.exec();
	return exitCode;
}
