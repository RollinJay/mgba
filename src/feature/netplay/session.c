/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/feature/netplay.h>

#include <gekkonet.h>

void mNetplaySessionInit(struct mNetplaySession* s, struct GBALinkMachine* machine, const struct mNetplayProfile* profile, struct GekkoSession* gekko) {
	memset(s, 0, sizeof(*s));
	s->machine = machine;
	s->profile = profile;
	s->gekko = gekko;
	s->stats.frame = -1;
	s->checksum = GBALinkMachineChecksum(machine);
}

void mNetplaySessionEnableTrace(struct mNetplaySession* s, int maxFrames) {
	free(s->traceChecksum);
	free(s->traceKeys);
	s->traceLength = maxFrames;
	s->traceChecksum = calloc(maxFrames, sizeof(*s->traceChecksum));
	s->traceKeys = calloc((size_t) maxFrames * NETPLAY_MAX_PLAYERS, sizeof(*s->traceKeys));
}

void mNetplaySessionDeinit(struct mNetplaySession* s) {
	free(s->traceChecksum);
	free(s->traceKeys);
	s->traceChecksum = NULL;
	s->traceKeys = NULL;
	s->traceLength = 0;
}

static void _advance(struct mNetplaySession* s, const struct GekkoGameEvent* ev) {
	const struct mNetplayProfile* profile = s->profile;
	uint16_t keys[NETPLAY_MAX_PLAYERS] = { 0 };
	int i;
	for (i = 0; i < profile->players && (unsigned) (i + 1) * NETPLAY_INPUT_SIZE <= ev->data.adv.input_len; ++i) {
		keys[i] = mNetplayDecodeKeys(&ev->data.adv.inputs[i * NETPLAY_INPUT_SIZE]);
	}
	/* frames GekkoNet re-simulates (or runs ahead speculatively) are not shown */
	bool hidden = ev->data.adv.rolling_back || ev->data.adv.running_ahead;
	if (hidden) {
		GBALinkMachineSetRender(s->machine, false);
	}
	mNetplayRunFrame(s->machine, profile, keys);
	if (hidden) {
		GBALinkMachineSetRender(s->machine, true);
	}
	if (s->frameCallback) {
		s->frameCallback(s, !hidden, s->frameContext);
	}
	if (ev->data.adv.rolling_back) {
		++s->stats.resimFrames;
	}
	s->stats.frame = ev->data.adv.frame;
	s->checksum = GBALinkMachineChecksum(s->machine);
	int f = ev->data.adv.frame;
	if (s->traceChecksum && f >= 0 && f < s->traceLength) {
		s->traceChecksum[f] = s->checksum;
		memcpy(&s->traceKeys[f * NETPLAY_MAX_PLAYERS], keys, sizeof(keys));
	}
}

void mNetplaySessionHandleEvents(struct mNetplaySession* s, struct GekkoGameEvent** events, int count) {
	int i;
	for (i = 0; i < count; ++i) {
		struct GekkoGameEvent* ev = events[i];
		switch (ev->type) {
		case GekkoSaveEvent:
			GBALinkMachineSaveState(s->machine, ev->data.save.state);
			*ev->data.save.state_len = (unsigned) GBALinkMachineStateSize(s->machine);
			*ev->data.save.checksum = s->checksum;
			break;
		case GekkoLoadEvent:
			GBALinkMachineLoadState(s->machine, ev->data.load.state);
			s->checksum = GBALinkMachineChecksum(s->machine);
			s->stats.frame = ev->data.load.frame;
			++s->stats.rollbacks;
			break;
		case GekkoAdvanceEvent:
			_advance(s, ev);
			break;
		default:
			break;
		}
	}
}
