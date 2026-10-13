/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/feature/netplay.h>

#include <mgba/core/core.h>
#include <mgba/core/version.h>
#include <mgba-util/configuration.h>
#include <mgba-util/sha1.h>
#include <mgba-util/string.h>
#include <mgba-util/vfs.h>

#include <ctype.h>
#include <errno.h>

#define DEFAULT_RTC_EPOCH_MS 978307200000LL   /* 2001-01-01T00:00:00Z */

static const struct {
	const char* name;
	uint16_t bit;
} _keyNames[] = {
	{ "A", 1 << 0 }, { "B", 1 << 1 }, { "SELECT", 1 << 2 }, { "START", 1 << 3 }, { "RIGHT", 1 << 4 },
	{ "LEFT", 1 << 5 }, { "UP", 1 << 6 }, { "DOWN", 1 << 7 }, { "R", 1 << 8 }, { "L", 1 << 9 },
};

bool mNetplayParseKeys(const char* text, uint16_t* keys) {
	*keys = 0;
	char word[16];
	size_t len = 0;
	const char* p = text;
	while (true) {
		char c = *p++;
		if (c == '+' || c == '\0') {
			while (len && isspace((unsigned char) word[len - 1])) {
				--len;
			}
			word[len] = '\0';
			if (len) {
				size_t i;
				bool found = false;
				for (i = 0; i < sizeof(_keyNames) / sizeof(*_keyNames); ++i) {
					if (strcasecmp(word, _keyNames[i].name) == 0) {
						*keys |= _keyNames[i].bit;
						found = true;
						break;
					}
				}
				if (!found) {
					return false;
				}
			}
			len = 0;
			if (!c) {
				return true;
			}
			continue;
		}
		if (isspace((unsigned char) c) && !len) {
			continue;
		}
		if (len >= sizeof(word) - 1) {
			return false;
		}
		word[len++] = c;
	}
}

/* ${VAR} expansion and resolution against the profile's folder. */
static bool _resolvePath(const char* in, const char* base, char* out, size_t outlen, char* err, size_t errlen) {
	char expanded[PATH_MAX];
	size_t o = 0;
	const char* p = in;
	while (*p) {
		if (p[0] == '$' && p[1] == '{') {
			const char* end = strchr(p + 2, '}');
			if (!end) {
				snprintf(err, errlen, "unterminated ${ in %s", in);
				return false;
			}
			char var[128];
			size_t vlen = end - (p + 2);
			if (vlen >= sizeof(var)) {
				vlen = sizeof(var) - 1;
			}
			memcpy(var, p + 2, vlen);
			var[vlen] = '\0';
			const char* val = getenv(var);
			if (!val) {
				snprintf(err, errlen, "environment variable %s is not set", var);
				return false;
			}
			size_t l = strlen(val);
			if (o + l >= sizeof(expanded)) {
				snprintf(err, errlen, "path too long: %s", in);
				return false;
			}
			memcpy(expanded + o, val, l);
			o += l;
			p = end + 1;
			continue;
		}
		if (o + 1 >= sizeof(expanded)) {
			snprintf(err, errlen, "path too long: %s", in);
			return false;
		}
		expanded[o++] = *p++;
	}
	expanded[o] = '\0';
	bool absolute = expanded[0] == '/' || expanded[0] == '\\' || (expanded[0] && expanded[1] == ':');
	size_t needed;
	if (absolute || !base[0]) {
		needed = strlcpy(out, expanded, outlen);
	} else {
		int n = snprintf(out, outlen, "%s" PATH_SEP "%s", base, expanded);
		needed = n < 0 ? outlen : (size_t) n;
	}
	if (needed >= outlen) {
		snprintf(err, errlen, "path too long: %s", in);
		return false;
	}
	return true;
}

static const char* _value(const struct Configuration* cfg, const char* section, const char* key) {
	const char* v = ConfigurationGetValue(cfg, section, key);
	return v && v[0] ? v : NULL;
}

static bool _parseInt(const char* text, long long* out) {
	char* end;
	errno = 0;
	long long v = strtoll(text, &end, 0);
	while (*end && isspace((unsigned char) *end)) {
		++end;
	}
	if (errno || end == text || *end) {
		return false;
	}
	*out = v;
	return true;
}

/* "2001-01-01T00:00:00Z" (UTC only) or a number of milliseconds since 1970. */
static bool _parseTime(const char* text, int64_t* ms) {
	int y, mo, d, h, mi, s;
	if (sscanf(text, "%d-%d-%dT%d:%d:%dZ", &y, &mo, &d, &h, &mi, &s) == 6) {
		/* days from civil (proleptic Gregorian) */
		y -= mo <= 2;
		int era = (y >= 0 ? y : y - 399) / 400;
		unsigned yoe = (unsigned) (y - era * 400);
		unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
		unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
		int64_t days = (int64_t) era * 146097 + (int64_t) doe - 719468;
		*ms = ((days * 24 + h) * 60 + mi) * 60000LL + s * 1000LL;
		return true;
	}
	long long v;
	if (_parseInt(text, &v)) {
		*ms = v;
		return true;
	}
	return false;
}

struct _introCollect {
	struct mNetplayProfile* profile;
	int maxIndex;
	bool bad;
	char badKey[64];
	const char* values[NETPLAY_MAX_INTRO + 1];
};

static void _collectIntro(const char* key, const char* value, void* user) {
	struct _introCollect* c = user;
	long long idx;
	if (!_parseInt(key, &idx) || idx < 1 || idx > NETPLAY_MAX_INTRO) {
		c->bad = true;
		strlcpy(c->badKey, key, sizeof(c->badKey));
		return;
	}
	c->values[idx] = value;
	if (idx > c->maxIndex) {
		c->maxIndex = (int) idx;
	}
}

/* "frames" or "frames: keys1, keys2, ..." (one entry per player, empty for none) */
static bool _parseStep(const char* text, struct mNetplayIntroStep* step, int players) {
	memset(step, 0, sizeof(*step));
	char buf[256];
	strlcpy(buf, text, sizeof(buf));
	char* colon = strchr(buf, ':');
	if (colon) {
		*colon = '\0';
	}
	long long frames;
	if (!_parseInt(buf, &frames) || frames < 0) {
		return false;
	}
	step->frames = (int) frames;
	if (!colon) {
		return true;
	}
	char* p = colon + 1;
	int player = 0;
	while (true) {
		char* comma = strchr(p, ',');
		if (comma) {
			*comma = '\0';
		}
		if (player >= players) {
			/* keys beyond the last player are only allowed if empty */
			char* q = p;
			while (*q && isspace((unsigned char) *q)) {
				++q;
			}
			if (*q) {
				return false;
			}
		} else if (!mNetplayParseKeys(p, &step->keys[player])) {
			return false;
		}
		++player;
		if (!comma) {
			break;
		}
		p = comma + 1;
	}
	return true;
}

bool mNetplayProfileLoad(struct mNetplayProfile* profile, const char* path, char* err, size_t errlen) {
	memset(profile, 0, sizeof(*profile));
	struct Configuration cfg;
	ConfigurationInit(&cfg);
	bool ok = false;
	if (!ConfigurationRead(&cfg, path)) {
		snprintf(err, errlen, "%s: cannot read", path);
		goto done;
	}
	char base[PATH_MAX];
	separatePath(path, base, NULL, NULL);

	const char* v;
	long long n;
	strlcpy(profile->name, (v = _value(&cfg, "profile", "name")) ? v : "", sizeof(profile->name));
	if (!(v = _value(&cfg, "profile", "gbas")) || !_parseInt(v, &n) || n < 1 || n > GBA_LINK_MACHINE_MAX) {
		snprintf(err, errlen, "%s: [profile] gbas must be 1 to %d", path, GBA_LINK_MACHINE_MAX);
		goto done;
	}
	profile->gbas = (int) n;
	profile->linked = (v = _value(&cfg, "profile", "link")) && _parseInt(v, &n) && n;
	if (profile->linked && profile->gbas < 2) {
		snprintf(err, errlen, "%s: a link needs at least 2 GBAs", path);
		goto done;
	}
	profile->rtcEpochMs = DEFAULT_RTC_EPOCH_MS;
	if ((v = _value(&cfg, "profile", "rtc")) && !_parseTime(v, &profile->rtcEpochMs)) {
		snprintf(err, errlen, "%s: bad rtc %s (use 2001-01-01T00:00:00Z)", path, v);
		goto done;
	}
	profile->screens = NETPLAY_SCREENS_OWN;
	if ((v = _value(&cfg, "profile", "screens"))) {
		if (strcasecmp(v, "all") == 0) {
			profile->screens = NETPLAY_SCREENS_ALL;
		} else if (strcasecmp(v, "own") != 0) {
			snprintf(err, errlen, "%s: screens must be own or all", path);
			goto done;
		}
	}

	int i;
	for (i = 0; i < profile->gbas; ++i) {
		char key[24];   /* "save." and any int */
		snprintf(key, sizeof(key), "rom.%d", i + 1);
		if (!(v = _value(&cfg, "profile", key)) && !(v = _value(&cfg, "profile", "rom"))) {
			snprintf(err, errlen, "%s: no rom (or %s) for GBA %d", path, key, i + 1);
			goto done;
		}
		char e[256];
		if (!_resolvePath(v, base, profile->roms[i], sizeof(profile->roms[i]), e, sizeof(e))) {
			snprintf(err, errlen, "%s: GBA %d rom: %s", path, i + 1, e);
			goto done;
		}
		snprintf(key, sizeof(key), "save.%d", i + 1);
		if ((v = _value(&cfg, "profile", key)) || (v = _value(&cfg, "profile", "save"))) {
			if (!_resolvePath(v, base, profile->saves[i], sizeof(profile->saves[i]), e, sizeof(e))) {
				snprintf(err, errlen, "%s: GBA %d save: %s", path, i + 1, e);
				goto done;
			}
		}
	}

	for (i = 0; i < NETPLAY_MAX_PLAYERS; ++i) {
		char section[16];
		snprintf(section, sizeof(section), "player.%d", i + 1);
		if (!(v = _value(&cfg, section, "gba"))) {
			break;
		}
		struct mNetplayPlayer* pl = &profile->player[i];
		if (!_parseInt(v, &n) || n < 1 || n > profile->gbas) {
			snprintf(err, errlen, "%s: [%s] gba must be 1 to %d", path, section, profile->gbas);
			goto done;
		}
		pl->gba = (int) n - 1;
		v = _value(&cfg, section, "input");
		if (!v || strcasecmp(v, "keypad") == 0) {
			pl->input = NETPLAY_INPUT_KEYPAD;
		} else if (strcasecmp(v, "mailbox") == 0) {
			pl->input = NETPLAY_INPUT_MAILBOX;
			static const char* fields[] = { "mailbox", "keysOffset", "markerOffset", "marker" };
			long long vals[4] = { 0 };
			int f;
			for (f = 0; f < 4; ++f) {
				const char* fv = _value(&cfg, section, fields[f]);
				if (fv && !_parseInt(fv, &vals[f])) {
					snprintf(err, errlen, "%s: [%s] bad %s", path, section, fields[f]);
					goto done;
				}
				if (!fv && f == 0) {
					snprintf(err, errlen, "%s: [%s] needs a mailbox address", path, section);
					goto done;
				}
			}
			pl->mailbox = (uint32_t) vals[0];
			pl->keysOffset = (uint32_t) vals[1];
			pl->markerOffset = (uint32_t) vals[2];
			pl->marker = (uint16_t) vals[3];
		} else {
			snprintf(err, errlen, "%s: [%s] input must be keypad or mailbox", path, section);
			goto done;
		}
	}
	profile->players = i;
	if (!profile->players) {
		snprintf(err, errlen, "%s: no [player.1]", path);
		goto done;
	}

	struct _introCollect c = { .profile = profile };
	ConfigurationEnumerate(&cfg, "intro", _collectIntro, &c);
	if (c.bad) {
		snprintf(err, errlen, "%s: [intro] keys must be step numbers 1 to %d, not %s", path, NETPLAY_MAX_INTRO, c.badKey);
		goto done;
	}
	for (i = 1; i <= c.maxIndex; ++i) {
		if (!c.values[i]) {
			snprintf(err, errlen, "%s: [intro] step %d is missing", path, i);
			goto done;
		}
		if (!_parseStep(c.values[i], &profile->intro[i - 1], profile->players)) {
			snprintf(err, errlen, "%s: [intro] step %d: bad value %s", path, i, c.values[i]);
			goto done;
		}
	}
	profile->introSteps = c.maxIndex;
	ok = true;
done:
	ConfigurationDeinit(&cfg);
	return ok;
}

int mNetplayProfileIntroFrames(const struct mNetplayProfile* profile) {
	int total = 0;
	int i;
	for (i = 0; i < profile->introSteps; ++i) {
		total += profile->intro[i].frames;
	}
	return total;
}

void mNetplayProfileIntroKeys(const struct mNetplayProfile* profile, int frame, uint16_t* keys) {
	memset(keys, 0, sizeof(uint16_t) * NETPLAY_MAX_PLAYERS);
	int i;
	for (i = 0; i < profile->introSteps; ++i) {
		if (frame < profile->intro[i].frames) {
			memcpy(keys, profile->intro[i].keys, sizeof(uint16_t) * NETPLAY_MAX_PLAYERS);
			return;
		}
		frame -= profile->intro[i].frames;
	}
}

static bool _hashFile(const char* path, uint8_t digest[20]) {
	struct VFile* vf = VFileOpen(path, O_RDONLY);
	if (!vf) {
		return false;
	}
	bool ok = sha1File(vf, digest);
	vf->close(vf);
	return ok;
}

static void _hex(const uint8_t* in, size_t len, char* out) {
	static const char digits[] = "0123456789abcdef";
	size_t i;
	for (i = 0; i < len; ++i) {
		out[i * 2] = digits[in[i] >> 4];
		out[i * 2 + 1] = digits[in[i] & 0xF];
	}
	out[len * 2] = '\0';
}

bool mNetplayProfileIdentity(const struct mNetplayProfile* profile, char identity[41], char* err, size_t errlen) {
	struct SHA1Context ctx;
	sha1Init(&ctx);
	char line[512];
	snprintf(line, sizeof(line), "mgba-netplay identity v1\nbuild %s %s %s\nlink %d\nrtc %lld\n", projectName,
	         projectVersion, gitCommit, profile->linked, (long long) profile->rtcEpochMs);
	sha1Update(&ctx, line, strlen(line));
	int i;
	for (i = 0; i < profile->gbas; ++i) {
		uint8_t digest[20];
		char rom[41], save[41] = "none";
		if (!_hashFile(profile->roms[i], digest)) {
			snprintf(err, errlen, "cannot read %s", profile->roms[i]);
			return false;
		}
		_hex(digest, 20, rom);
		if (profile->saves[i][0]) {
			if (!_hashFile(profile->saves[i], digest)) {
				snprintf(err, errlen, "cannot read %s", profile->saves[i]);
				return false;
			}
			_hex(digest, 20, save);
		}
		snprintf(line, sizeof(line), "gba %d rom %s save %s\n", i, rom, save);
		sha1Update(&ctx, line, strlen(line));
	}
	for (i = 0; i < profile->players; ++i) {
		const struct mNetplayPlayer* pl = &profile->player[i];
		snprintf(line, sizeof(line), "player %d gba %d input %d mailbox %x %x %x %x\n", i, pl->gba, pl->input,
		         pl->mailbox, pl->keysOffset, pl->markerOffset, pl->marker);
		sha1Update(&ctx, line, strlen(line));
	}
	uint8_t digest[20];
	sha1Finalize(digest, &ctx);
	_hex(digest, 20, identity);
	return true;
}

static void* _readFile(const char* path, size_t* size) {
	struct VFile* vf = VFileOpen(path, O_RDONLY);
	if (!vf) {
		return NULL;
	}
	ssize_t len = vf->size(vf);
	void* data = NULL;
	if (len > 0) {
		data = malloc(len);
		if (vf->read(vf, data, len) != len) {
			free(data);
			data = NULL;
		} else {
			*size = len;
		}
	}
	vf->close(vf);
	return data;
}

struct GBALinkMachine* mNetplayProfileOpen(const struct mNetplayProfile* profile, char* err, size_t errlen) {
	struct GBALinkMachineConfig cfg = {
		.gbas = profile->gbas,
		.linked = profile->linked,
		.rtcEpochMs = profile->rtcEpochMs,
	};
	void* saves[GBA_LINK_MACHINE_MAX] = { 0 };
	struct GBALinkMachine* m = NULL;
	int i;
	for (i = 0; i < profile->gbas; ++i) {
		cfg.roms[i] = profile->roms[i];
		if (profile->saves[i][0]) {
			saves[i] = _readFile(profile->saves[i], &cfg.saveSizes[i]);
			if (!saves[i]) {
				snprintf(err, errlen, "cannot read %s", profile->saves[i]);
				goto done;
			}
			cfg.saves[i] = saves[i];
		}
	}
	m = GBALinkMachineOpen(&cfg, err, errlen);
done:
	for (i = 0; i < GBA_LINK_MACHINE_MAX; ++i) {
		free(saves[i]);
	}
	return m;
}

void mNetplayRunFrame(struct GBALinkMachine* m, const struct mNetplayProfile* profile, const uint16_t* players) {
	uint16_t keys[GBA_LINK_MACHINE_MAX] = { 0 };
	int i;
	for (i = 0; i < profile->players; ++i) {
		const struct mNetplayPlayer* pl = &profile->player[i];
		uint16_t k = players[i] & 0x3FF;
		switch (pl->input) {
		case NETPLAY_INPUT_KEYPAD:
			keys[pl->gba] |= k;
			break;
		case NETPLAY_INPUT_MAILBOX:
			GBALinkMachineWrite16(m, pl->gba, pl->mailbox + pl->keysOffset, k);
			if (pl->marker) {
				GBALinkMachineWrite16(m, pl->gba, pl->mailbox + pl->markerOffset, pl->marker);
			}
			break;
		}
	}
	GBALinkMachineRunFrame(m, keys);
}
