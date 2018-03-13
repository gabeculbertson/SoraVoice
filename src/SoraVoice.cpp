#include "SoraVoice.h"

#include <TextHook.h>

#include <SVData.h>

#include <Utils/Log.h>
#include <Utils/Clock.h>
#include <Utils/build_date.h>

#include <Message.h>
#include <Hard/Mapping.h>
#include <Config.h>

#include <Hook/Hook.h>
#include <Player/Player.h>
#include <Draw/Draw.h>

#include <string>
#include <vector>
#include <mutex>
#include <random>
#include <ctime>
#include <cstring>

#include <sstream>
#include <iostream>
#include <iomanip>

#include <stdlib.h>

#include <dinput.h>

//#using <SoraVoiceLib.dll>
//#include <LineToVoice\voice_map.h>

#ifndef DID
#define DID IDirectInputDevice
#endif // !DID

using namespace std;

using LockGuard = std::lock_guard<std::mutex>;
using Draw::InfoType;
using Player::PlayID;
using Player::StopType;
using byte = unsigned char;

#ifdef TEST_VER
#define STR_VERSION { 'T', 'E', 'S', 'T', ' ', BUILD_DATE, '\0' }
#elif defined(DBG_VER)
#define STR_VERSION { 'D', 'E', 'B', 'U', 'G', ' ', BUILD_DATE, '\0' }
#else
#define STR_VERSION { BUILD_DATE, '\0' }
#endif // TEST_VER

static const char DateVersion[] = STR_VERSION;

constexpr int ORIVOICEID_LEN = 4;
constexpr int BGMVOICEID_LEN = 6;
static_assert(BGMVOICEID_LEN > MAX_VOICEID_LEN_NEED_MAPPING, "BGMVOICEID_LEN <= MAX_VOICEID_LEN_NEED_MAPPING !!!");

constexpr char ORIVOICEFILE_PREFIX[] = "data\\se\\ed7v";
constexpr char ORIVOICEFILE_ATTR[] = ".wav";
constexpr char CONFIG_FILE[] = DFT_CONFIG_FILE;
constexpr char VOICEFILE_DIR[] = "voice\\ogg\\";
constexpr char VOICEFILE_PREFIX_ZA[] = "v";
constexpr char VOICEFILE_PREFIX_ED6[] = "ch";
constexpr char VOICEFILE_PREFIX_BGM[] = "ed";
constexpr char VOICEFILE_ATTR[] = ".ogg";

constexpr int VOLUME_STEP = 1;
constexpr int VOLUME_STEP_BIG = 5;

constexpr int KEY_VOLUME_UP = DIK_EQUALS;
constexpr int KEY_VOLUME_DOWN = DIK_MINUS;
constexpr int KEY_VOLUME_BIGSTEP1 = DIK_LSHIFT;
constexpr int KEY_VOLUME_BIGSTEP2 = DIK_RSHIFT;

constexpr int ORIVOLPCT_STEP = 10;
constexpr int KEY_ORIVOLPCT_UP = DIK_RBRACKET;
constexpr int KEY_ORIVOLPCT_DOWN = DIK_LBRACKET;

constexpr int KEY_ORIVOICE = DIK_BACKSPACE;
constexpr int KEY_AUTOPLAY = DIK_0;
constexpr int KEY_SKIPVOICE = DIK_9;
constexpr int KEY_DLGSE = DIK_8;
constexpr int KEY_DU = DIK_7;
constexpr int KEY_INFOONOFF = DIK_6;
constexpr int KEY_ALLINFO = DIK_BACKSLASH;
constexpr int KEY_RESETA = DIK_LBRACKET;
constexpr int KEY_RESETB = DIK_RBRACKET;

constexpr unsigned INFO_TIME = 3000;
constexpr unsigned HELLO_TIME = 8000;
constexpr unsigned INFINITY_TIME = Draw::ShowTimeInfinity;
constexpr unsigned REMAIN_TIME = 2000;

constexpr unsigned TIME_PREC = 16;

constexpr int KEYS_NUM = 256;

std::string game_names[] = { "invalid", "ed6fc", "ed6sc", "ed6t3" };

struct Keys {
	const byte* const &keys;
	DID* const pDID;
	byte last[KEYS_NUM]{};
	Keys(const byte* &keys, void* pDID)
		:keys(keys), pDID((decltype(this->pDID))pDID) {
	}
};

struct AutoPlay {
	const unsigned &now;

	unsigned &count_ch;
	unsigned &wait;
	unsigned &time_textbeg;
	unsigned time_autoplay = 0;

	unsigned &waitv;
	unsigned time_autoplayv = 0;

	AutoPlay(unsigned& now, unsigned &count_ch,
		unsigned &wait, unsigned &time_textbeg,
		unsigned &waitv)
		:now(now),
		count_ch(count_ch), wait(wait), time_textbeg(time_textbeg),
		waitv(waitv) {
	}
};

static Keys * VC_keys;
static AutoPlay * VC_aup;
static std::mutex VC_mt_playID;
static PlayID VC_curPlayID;

static bool VC_isZa;

static const char *VOICEFILE_PREFIX;

static void playRandomVoice(const char* vlist) {
	if (!vlist) return;

	std::vector<const char*> vl;
	while (*vlist) {
		vl.push_back(vlist);
		while (*vlist) vlist++;
		vlist++;
	}

	LOG("Random voice num : %d", vl.size());
	if (!vl.empty()) {
		std::default_random_engine random((unsigned)std::time(nullptr));
		std::uniform_int_distribution<int> dist(0, vl.size() - 1);

		LOG("Play Random voice.");
		int volume = Config.Volume * Config.OriVolumePercent / 120;
		if (volume > Config.MAX_Volume) volume = Config.MAX_Volume;
		Player::Play(vl[dist(random)], volume);
	}
}

inline static bool isAutoPlaying() {
	return VC_aup->count_ch
		&& ((Config.AutoPlay && (SV.status.playing || VC_aup->waitv))
			|| Config.AutoPlay == CConfig::AutoPlay_ALL);
}

inline static std::string GetStr(const char* first) {
	return first;
}

inline static std::string GetStr(char* first) {
	return first;
}

template<typename First, typename = std::enable_if_t<std::is_integral_v<First>>>
inline static std::string GetStr(First first) {
	return std::to_string(first);
}

template<typename First, typename... Remain>
inline static std::string GetStr(First first, Remain... remains) {
	return GetStr(first) + GetStr(remains...);
}

template<typename... Texts>
inline static unsigned AddInfo(InfoType type, unsigned time, unsigned color, Texts... texts) {
	return Draw::AddInfo(type, time, color, GetStr(texts...).c_str());
}

static void stopCallBack(PlayID playID, StopType stopType)
{
	LOG("StopCallBack: playID = 0x%08d, stopType = %d", playID, (int)stopType);
	LockGuard lock(VC_mt_playID);
	if (playID == VC_curPlayID) {
		if (stopType == StopType::PlayEnd) {
			VC_aup->waitv = 1;
			VC_aup->time_autoplayv = VC_aup->now + Config.WaitTimeDialogVoice - TIME_PREC / 2;
		}
		else {
			SV.order.disableDududu = 0;
			SV.order.disableDialogSE = 0;
		}
		SV.status.playing = 0;
	} //if (playID == this->playID)
}

constexpr char hexmap[] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F' };
std::string ToHexStringKey(const byte* data, int len)
{
	std::string s;
	int inPrefix = 0;
	for (int i = 0; i <= len; i++) {
		if (data[i] == 0x02) break;
		if (data[i] < 0x20) continue;
		s.push_back(hexmap[(data[i] & 0xF0) >> 4]);
		s.push_back(hexmap[data[i] & 0x0F]);
	}
	return s;
}

std::string lastPlayed = "";
int lastCharPointer = 0;
void SoraVoice::Play(const char* t)
{
	if (!SV.status.startup) return;

	bool isOneStepForward = abs((int)t - lastCharPointer) <= 4;
	lastCharPointer = (int)t;

	std::string str_utterance;
	for (int i = 0; i < MAX_TEXTBOX_STRING_LEN; i++) {
		if (*(t + i) == 0x02) break;
		str_utterance.push_back(*(t + i));
	}
	std::string str_matched_vid = ToHexStringKey((byte*)t, str_utterance.length());

	LOG("scene is: %d     %d", lastCharPointer, (int)t);

	std::string str_vid;
	if (globalSceneLines.count(str_matched_vid) > 0) {
		if (lastPlayed == str_matched_vid) return;

		str_vid = globalSceneLines[str_matched_vid];
		lastPlayed = str_matched_vid;
		TextHook::HandleText(game_names[SV.game], str_utterance, str_vid);

		LOG("scene is: \"%s\"", globalScene.c_str());
		LOG("input text is: \"%s\"", str_utterance.c_str());
		LOG("input bytes is: \"%s\"", str_matched_vid.c_str());
	}
	else if (!isOneStepForward) {
		TextHook::HandleText(game_names[SV.game], str_utterance, "");

		LOG("scene is: \"%s\"", globalScene.c_str());
		LOG("input text is: \"%s\"", str_utterance.c_str());
		LOG("input bytes is: \"%s\"", str_matched_vid.c_str());

		return;
	}
	else {
		if (*t != '#') return;
		t++;

		unsigned num_vid = 0;
		for (int i = 0; i < MAX_VOICEID_LEN; i++) {
			if (*t < '0' || *t > '9') break;
			num_vid *= 10; num_vid += *t - '0';
			str_vid.push_back(*t);

			t++;
		}
		if (*t != 'v' || str_vid.empty()) return;

		LOG("input Voice ID is \"%s\"", str_vid.c_str());
		LOG("The max length of voice id need mapping is %d", MAX_VOICEID_LEN_NEED_MAPPING);

		if (str_vid.length() <= MAX_VOICEID_LEN_NEED_MAPPING) {
			if (VC_isZa) {
				LOG("Voice ID mapping is not supported in Zero/Ao, return");
				return;
			}

			num_vid += VoiceIdAdjustAdder[str_vid.length()];
			LOG("Adjusted Voice ID is %d", num_vid);
			LOG("Number of mapping is %d", NUM_MAPPING);

			if (num_vid >= NUM_MAPPING) {
				LOG("Adjusted Voice ID is out of the range of Mapping");
				return;
			}

			str_vid = VoiceIdMapping[num_vid];
			if (str_vid.empty()) {
				LOG("Mapping Voice ID is empty");
				return;
			}
		}
	}

	std::string oggFileName = VOICEFILE_DIR;
	if (str_vid.length() == BGMVOICEID_LEN) {
		oggFileName.append(VOICEFILE_PREFIX_BGM).append(str_vid.c_str() + sizeof(VOICEFILE_PREFIX_BGM) - 1);
	}
	else {
		oggFileName.append(VOICEFILE_PREFIX).append(str_vid);
	}
	oggFileName.append(VOICEFILE_ATTR);

	int volume = Config.Volume;

	if (VC_isZa) {
		SV.status.playingOri = 0;
		if (Config.OriginalVoice) {
			++t;
			while (*t == '#') {
				t++;
				const char* p = t;
				while (*p >= '0' && *p <= '9') p++;
				if (*p == 'V' && p - t == ORIVOICEID_LEN) {
					if (Config.OriginalVoice == CConfig::OriginalVoice_OriOnly) {
						oggFileName = ORIVOICEFILE_PREFIX + str_vid.assign(t, ORIVOICEID_LEN) + ORIVOICEFILE_ATTR;
						SV.status.playingOri = 1;
						volume = Config.Volume * Config.OriVolumePercent / 100;
						if (volume > Config.MAX_Volume) volume = Config.MAX_Volume;
					}
					*(unsigned*)t = 0x39393939;
				}
				t = p + 1;
			}
		}
	}

	LOG("Sound filename: %s", oggFileName.c_str());

	LOG("Now playing new file...");
	{
		LockGuard lock(VC_mt_playID);

		SV.status.playing = 1;
		VC_curPlayID = Player::Play(oggFileName.c_str(), SV.status.mute ? 0 : volume);
	}

	SV.order.disableDududu = Config.DisableDududu;
	SV.order.disableDialogSE = Config.DisableDialogSE;

	LOG("Play called, playID = 0x%08X", VC_curPlayID);
}

void SoraVoice::Stop()
{
	if (!SV.status.startup) return;

	LOG("Stop is called.");

	if (Config.ShowInfo == CConfig::ShowInfo_WithMark && isAutoPlaying()) {
		AddInfo(InfoType::AutoPlayMark, REMAIN_TIME, Config.FontColor, Message.AutoPlayMark);
	}

	if (Config.SkipVoice) {
		Player::Stop();
	}
	SV.status.playing = 0;

	SV.order.disableDududu = 0;
	SV.order.disableDialogSE = 0;

	VC_aup->wait = 0;
	VC_aup->waitv = 0;
	VC_aup->count_ch = 0;
	VC_aup->time_autoplay = 0;
}

void SoraVoice::Input()
{
	if (!Config.EnableKeys || !VC_keys->keys) return;

	auto keys = VC_keys->keys;
	auto last = VC_keys->last;

	bool needsave = false;
	bool needsetvolume = false;
	int volume_old = Config.Volume;

	unsigned info_time = INFO_TIME;

	if (keys[KEY_ALLINFO]) {
		info_time = INFINITY_TIME;
		if (!last[KEY_ALLINFO]) {
			Draw::RemoveInfo(InfoType::Volume);
			if (SV.game == AO) {
				Draw::RemoveInfo(InfoType::OriVolumePercent);
				Draw::RemoveInfo(InfoType::OriginalVoice);
			}

			Draw::RemoveInfo(InfoType::AutoPlay);
			Draw::RemoveInfo(InfoType::SkipVoice);
			Draw::RemoveInfo(InfoType::DisableDialogSE);
			Draw::RemoveInfo(InfoType::DisableDududu);
			Draw::RemoveInfo(InfoType::InfoOnoff);

			if (SV.status.mute) AddInfo(InfoType::Volume, INFINITY_TIME, Config.FontColor, Message.Mute);
			else AddInfo(InfoType::Volume, INFINITY_TIME, Config.FontColor, Message.Volume, Config.Volume);

			if (SV.game == AO) {
				AddInfo(InfoType::OriVolumePercent, INFINITY_TIME, Config.FontColor, Message.OriVolumePercent, Config.OriVolumePercent, "%");
				AddInfo(InfoType::OriginalVoice, INFINITY_TIME, Config.FontColor, Message.OriginalVoice, Message.OriginalVoiceSwitch[Config.OriginalVoice]);
			}
			AddInfo(InfoType::AutoPlay, INFINITY_TIME, Config.FontColor, Message.AutoPlay, Message.AutoPlaySwitch[Config.AutoPlay]);
			AddInfo(InfoType::SkipVoice, INFINITY_TIME, Config.FontColor, Message.SkipVoice, Message.Switch[Config.SkipVoice]);
			AddInfo(InfoType::DisableDialogSE, INFINITY_TIME, Config.FontColor, Message.DisableDialogSE, Message.Switch[Config.DisableDialogSE]);
			AddInfo(InfoType::DisableDududu, INFINITY_TIME, Config.FontColor, Message.DisableDududu, Message.Switch[Config.DisableDududu]);
			AddInfo(InfoType::InfoOnoff, INFINITY_TIME, Config.FontColor, Message.ShowInfo, Message.ShowInfoSwitch[Config.ShowInfo]);
		}
	}
	else if (last[KEY_ALLINFO]) {
		Draw::RemoveInfo(InfoType::Volume);
		if (SV.game == AO) {
			Draw::RemoveInfo(InfoType::OriVolumePercent);
			Draw::RemoveInfo(InfoType::OriginalVoice);
		}
		Draw::RemoveInfo(InfoType::AutoPlay);
		Draw::RemoveInfo(InfoType::SkipVoice);
		Draw::RemoveInfo(InfoType::DisableDialogSE);
		Draw::RemoveInfo(InfoType::DisableDududu);
		Draw::RemoveInfo(InfoType::InfoOnoff);
	}//keys[KEY_ALLINFO]

	if ((keys[KEY_RESETA] && keys[KEY_RESETB])
		&& !(last[KEY_RESETA] && last[KEY_RESETB])) {
		if (Config.SaveChange) {
			Config.Reset();
			needsave = true;
			needsetvolume = true;
		}
		else {
			Config.LoadConfig(CONFIG_FILE);
			Config.EnableKeys = 1;
			Config.SaveChange = 0;
			needsetvolume = true;
		}
		SV.status.mute = 0;
		if (SV.status.playing) {
			SV.order.disableDialogSE = Config.DisableDialogSE;
			SV.order.disableDududu = Config.DisableDududu;
		}
		LOG("Reset config");

		if (Config.ShowInfo || info_time == INFINITY_TIME) {
			//Draw::AddText(InfoType::ConfigReset, INFO_TIME, config.FontColor, Message.Reset);
			AddInfo(InfoType::Volume, info_time, Config.FontColor, Message.Volume, Config.Volume);
			if (SV.game == AO) {
				AddInfo(InfoType::OriVolumePercent, info_time, Config.FontColor, Message.OriVolumePercent, Config.OriVolumePercent, "%");
				AddInfo(InfoType::OriginalVoice, info_time, Config.FontColor, Message.OriginalVoice, Message.OriginalVoiceSwitch[Config.OriginalVoice]);
			}
			AddInfo(InfoType::AutoPlay, info_time, Config.FontColor, Message.AutoPlay, Message.AutoPlaySwitch[Config.AutoPlay]);
			AddInfo(InfoType::SkipVoice, info_time, Config.FontColor, Message.SkipVoice, Message.Switch[Config.SkipVoice]);
			AddInfo(InfoType::DisableDialogSE, info_time, Config.FontColor, Message.DisableDialogSE, Message.Switch[Config.DisableDialogSE]);
			AddInfo(InfoType::DisableDududu, info_time, Config.FontColor, Message.DisableDududu, Message.Switch[Config.DisableDududu]);
			AddInfo(InfoType::InfoOnoff, info_time, Config.FontColor, Message.ShowInfo, Message.ShowInfoSwitch[Config.ShowInfo]);

			if (Config.ShowInfo == CConfig::ShowInfo_WithMark && isAutoPlaying()) {
				AddInfo(InfoType::AutoPlayMark, INFINITY_TIME, Config.FontColor, Message.AutoPlayMark);
			}
			else {
				Draw::RemoveInfo(InfoType::AutoPlayMark);
			}
		}
		else if (info_time != INFINITY_TIME) {
			Draw::RemoveInfo(InfoType::All);
		}
		else {
			Draw::RemoveInfo(InfoType::AutoPlayMark);
			Draw::RemoveInfo(InfoType::Hello);
		}
	} //keys[KEY_RESETA] && keys[KEY_RESETB]
	else {
		bool show_info = Config.ShowInfo || info_time == INFINITY_TIME;
		if (keys[KEY_VOLUME_UP] && !last[KEY_VOLUME_UP] && !keys[KEY_VOLUME_DOWN]) {
			if (keys[KEY_VOLUME_BIGSTEP1] || keys[KEY_VOLUME_BIGSTEP2]) Config.Volume += VOLUME_STEP_BIG;
			else Config.Volume += VOLUME_STEP;

			if (Config.Volume > CConfig::MAX_Volume) Config.Volume = CConfig::MAX_Volume;
			SV.status.mute = 0;
			needsetvolume = volume_old != Config.Volume;
			needsave = needsetvolume;

			if (show_info) {
				AddInfo(InfoType::Volume, info_time, Config.FontColor, Message.Volume, Config.Volume);
			}

			LOG("Set Volume : %d", Config.Volume);
		} //if(KEY_VOLUME_UP)
		else if (keys[KEY_VOLUME_DOWN] && !last[KEY_VOLUME_DOWN] && !keys[KEY_VOLUME_UP]) {
			if (keys[KEY_VOLUME_BIGSTEP1] || keys[KEY_VOLUME_BIGSTEP2]) Config.Volume -= VOLUME_STEP_BIG;
			else Config.Volume -= VOLUME_STEP;

			if (Config.Volume < 0) Config.Volume = 0;
			SV.status.mute = 0;
			needsetvolume = volume_old != Config.Volume;
			needsave = needsetvolume;

			if (show_info) {
				AddInfo(InfoType::Volume, info_time, Config.FontColor, Message.Volume, Config.Volume);
			}

			LOG("Set Volume : %d", Config.Volume);
		}//if(KEY_VOLUME_DOWN)
		else if (keys[KEY_VOLUME_UP] && keys[KEY_VOLUME_DOWN] && !(last[KEY_VOLUME_UP] && last[KEY_VOLUME_DOWN])) {
			SV.status.mute = 1;
			needsetvolume = true;

			if (show_info) {
				AddInfo(InfoType::Volume, info_time, Config.FontColor, Message.Mute);
			}

			LOG("Set mute : %d", SV.status.mute);
		}//keys[KEY_VOLUME_UP] && keys[KEY_VOLUME_DOWN]

		if (SV.game == AO) {
			if (keys[KEY_ORIVOLPCT_UP] && !last[KEY_ORIVOLPCT_UP] && !keys[KEY_ORIVOLPCT_DOWN]) {
				needsetvolume = Config.OriVolumePercent != ORIVOLPCT_STEP;
				needsave = needsetvolume;

				Config.OriVolumePercent += ORIVOLPCT_STEP;
				if (Config.OriVolumePercent > Config.MAX_OriVolumePercent) Config.OriVolumePercent = Config.MAX_OriVolumePercent;

				if (show_info) {
					AddInfo(InfoType::OriVolumePercent, info_time, Config.FontColor, Message.OriVolumePercent, Config.OriVolumePercent, "%");
				}

				LOG("Set OriVolumePercent : %d", Config.OriVolumePercent);
			} //keys[KEY_ORIVOLPCT_UP]
			else if (keys[KEY_ORIVOLPCT_DOWN] && !last[KEY_ORIVOLPCT_DOWN] && !keys[KEY_ORIVOLPCT_UP]) {
				needsetvolume = Config.OriVolumePercent != 0;
				needsave = needsetvolume;

				Config.OriVolumePercent -= ORIVOLPCT_STEP;
				if (Config.OriVolumePercent < 0) Config.OriVolumePercent = 0;

				if (show_info) {
					AddInfo(InfoType::OriVolumePercent, info_time, Config.FontColor, Message.OriVolumePercent, Config.OriVolumePercent, "%");
				}

				LOG("Set OriVolumePercent : %d", Config.OriVolumePercent);
			}

			if (keys[KEY_ORIVOICE] && !last[KEY_ORIVOICE]) {
				(Config.OriginalVoice += 1) %= (CConfig::MAX_OriginalVoice + 1);
				needsave = true;

				if (show_info) {
					AddInfo(InfoType::OriginalVoice, info_time, Config.FontColor, Message.OriginalVoice,
						Message.OriginalVoiceSwitch[Config.OriginalVoice]);
				}

				LOG("Set OriginalVoice : %d", Config.OriginalVoice);
			}//if(KEY_ORIVOICE)
		}

		if (keys[KEY_AUTOPLAY] && !last[KEY_AUTOPLAY]) {
			(Config.AutoPlay += 1) %= (CConfig::MAX_AutoPlay + 1);
			needsave = true;

			if (show_info) {
				AddInfo(InfoType::AutoPlay, info_time, Config.FontColor, Message.AutoPlay, Message.AutoPlaySwitch[Config.AutoPlay]);
				if (Config.ShowInfo == CConfig::ShowInfo_WithMark && isAutoPlaying()) {
					AddInfo(InfoType::AutoPlayMark, INFINITY_TIME, Config.FontColor, Message.AutoPlayMark);
				}
				else {
					Draw::RemoveInfo(InfoType::AutoPlayMark);
				}
			}
			LOG("Set AutoPlay : %d", Config.AutoPlay);

			if (Config.AutoPlay && !Config.SkipVoice) {
				Config.SkipVoice = 1;

				if (show_info) {
					AddInfo(InfoType::SkipVoice, info_time, Config.FontColor, Message.SkipVoice, Message.Switch[Config.SkipVoice]);
				}
				LOG("Set SkipVoice : %d", Config.SkipVoice);
			}
		}//if(KEY_AUTOPLAY)

		if (keys[KEY_SKIPVOICE] && !last[KEY_SKIPVOICE]) {
			Config.SkipVoice = 1 - Config.SkipVoice;
			needsave = true;

			if (show_info) {
				AddInfo(InfoType::SkipVoice, info_time, Config.FontColor, Message.SkipVoice, Message.Switch[Config.SkipVoice]);
			}

			LOG("Set SkipVoice : %d", Config.SkipVoice);

			if (!Config.SkipVoice && Config.AutoPlay) {
				Config.AutoPlay = 0;
				if (show_info) {
					AddInfo(InfoType::AutoPlay, info_time, Config.FontColor, Message.AutoPlay, Message.AutoPlaySwitch[Config.AutoPlay]);
				}
				Draw::RemoveInfo(InfoType::AutoPlayMark);
				LOG("Set AutoPlay : %d", Config.AutoPlay);
			}
		}//if(KEY_SKIPVOICE)

		if (keys[KEY_DLGSE] && !last[KEY_DLGSE]) {
			Config.DisableDialogSE = 1 - Config.DisableDialogSE;
			if (SV.status.playing) {
				SV.order.disableDialogSE = Config.DisableDialogSE;
			}
			needsave = true;

			if (show_info) {
				AddInfo(InfoType::DisableDialogSE, info_time, Config.FontColor, Message.DisableDialogSE, Message.Switch[Config.DisableDialogSE]);
			}

			LOG("Set DisableDialogSE : %d", Config.DisableDialogSE);
		}//if(KEY_DLGSE)

		if (keys[KEY_DU] && !last[KEY_DU]) {
			Config.DisableDududu = 1 - Config.DisableDududu;
			if (SV.status.playing) {
				SV.order.disableDududu = Config.DisableDududu;
			}
			needsave = true;

			if (show_info) {
				AddInfo(InfoType::DisableDududu, info_time, Config.FontColor, Message.DisableDududu, Message.Switch[Config.DisableDududu]);
			}

			LOG("Set DisableDududu : %d", Config.DisableDududu);
		}//if(KEY_DU)

		if (keys[KEY_INFOONOFF] && !last[KEY_INFOONOFF]) {
			Config.ShowInfo = (Config.ShowInfo + 1) % (CConfig::MAX_ShowInfo + 1);
			needsave = true;

			if (Config.ShowInfo) {
				if (Config.ShowInfo == CConfig::ShowInfo_WithMark && isAutoPlaying()) {
					AddInfo(InfoType::AutoPlayMark, INFINITY_TIME, Config.FontColor, Message.AutoPlayMark);
				}
				else {
					Draw::RemoveInfo(InfoType::AutoPlayMark);
				}
			}
			else if (info_time == INFINITY_TIME) {
				Draw::RemoveInfo(InfoType::AutoPlayMark);
				Draw::RemoveInfo(InfoType::Hello);
			}
			else {
				Draw::RemoveInfo(InfoType::All);
			}
			AddInfo(InfoType::InfoOnoff, info_time, Config.FontColor, Message.ShowInfo, Message.ShowInfoSwitch[Config.ShowInfo]);

			LOG("Set ShowInfo : %d", Config.ShowInfo);
		}//if(KEY_INFO)
	}

	if (needsetvolume) {
		if (SV.status.playing) {
			if (!SV.status.mute) {
				int volume = SV.status.playingOri ?
					Config.Volume * Config.OriVolumePercent / 100 :
					Config.Volume;
				if (volume > Config.MAX_Volume) volume = Config.MAX_Volume;
				Player::SetVolume(volume);
			}
			else Player::SetVolume(0);
		}
	}

	if (needsave && Config.SaveChange) {
		Config.SaveConfig(CONFIG_FILE);
		LOG("Config file saved");
	}

	std::memcpy(last, keys, KEYS_NUM);
}

void SoraVoice::Show(void* pD3DD)
{
	if (!SV.status.startup) return;

	Clock::UpdateTime();
	const auto& aup = VC_aup;

	if (!VC_isZa) SoraVoice::Input();

	if (SV.status.showing && Draw::RemoveInfo(InfoType::Dead)) {
		Draw::DrawInfos(pD3DD);
	}

	if (SV.status.first_text) {
		SV.status.first_text = 0;
		if (Config.ShowInfo == CConfig::ShowInfo_WithMark && isAutoPlaying()) {
			AddInfo(InfoType::AutoPlayMark, Draw::ShowTimeInfinity, Config.FontColor, Message.AutoPlayMark);
		}
	}

	if (!SV.status.playing) {
		if (aup->wait
			&& SV.status.scode != SV.scode.SAY && SV.status.scode != SV.scode.TALK && SV.status.scode != SV.scode.TEXT) {
			aup->count_ch = 0;
			aup->wait = 0;
			aup->waitv = 0;
			aup->time_autoplay = 0;

			if (Config.ShowInfo == CConfig::ShowInfo_WithMark) {
				Draw::RemoveInfo(InfoType::AutoPlayMark);
			}

			SV.order.disableDududu = 0;
		}
		else if (aup->wait && !aup->time_autoplay) {
			aup->time_autoplay = aup->time_textbeg
				+ aup->count_ch * Config.WaitTimePerChar + Config.WaitTimeDialog - TIME_PREC / 2;

			SV.order.disableDududu = 0;
		}

		if (aup->wait && aup->time_autoplay <= aup->now
			&& (!aup->waitv || aup->time_autoplayv <= aup->now)) {
			LOG("now = %d", aup->now);
			LOG("waitv = %d", aup->waitv);
			LOG("autoplayv = %d", aup->time_autoplayv);

			LOG("wait = %d", aup->wait);
			LOG("time_textbeg = %d", aup->time_textbeg);
			LOG("cnt = %d", aup->count_ch - 1);
			LOG("autoplay = %d", aup->time_autoplay);

			if (isAutoPlaying()) {
				SV.order.autoPlay = 1;
				LOG("Auto play set.");

				SetThreadExecutionState(ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);

				if (Config.ShowInfo == CConfig::ShowInfo_WithMark) {
					AddInfo(InfoType::AutoPlayMark, REMAIN_TIME, Config.FontColor, Message.AutoPlayMark);
				}
			}

			aup->count_ch = 0;
			aup->wait = 0;
			aup->waitv = 0;
			aup->time_autoplay = 0;
		}
	}
}

bool SoraVoice::Init() {
	if (SV.status.startup || SV.status.ended) return false;
	VC_isZa = SV.series == SERIES_ZEROAO;

	if (VC_isZa) {
		if (!InitSVData()) {
			LOG("Init SV failed.");
			return false;
		}
	}

	Config.SaveConfig(CONFIG_FILE);

	Clock::InitClock(SV.rcd.now, SV.rcd.recent);
	Draw::Init();
	Player::Init(*SV.addrs.p_pDS, stopCallBack);

	VC_keys = new Keys(SV.addrs.p_keys, *SV.addrs.p_did);
	VC_aup = new AutoPlay(SV.rcd.now, SV.rcd.count_ch, SV.status.wait,
		SV.rcd.time_textbeg, SV.status.waitv);

	static_assert(CConfig::MAX_Volume == Player::MaxVolume, "Max Volume not same!");

	if (VC_isZa) {
		if (Config.EnableKeys) {
			LOG("Now going to hook GetDeviceState...");
			void* pGetDeviceState = Hook::Hook_DI_GetDeviceState(*SV.addrs.p_did, SoraVoice::Input, (void**)&SV.addrs.p_keys);
			if (pGetDeviceState) {
				LOG("GetDeviceState hooked, old GetDeviceState = 0x%08X", (unsigned)pGetDeviceState);
			}
			else {
				LOG("Hook GetDeviceState failed.");
			}
		}
	}

	if (Config.ShowInfo) {
		AddInfo(InfoType::Hello, HELLO_TIME, Config.FontColor, Message.Title);
		AddInfo(InfoType::Hello, HELLO_TIME, Config.FontColor, Message.Version, DateVersion);
		AddInfo(InfoType::Hello, HELLO_TIME, Config.FontColor, Message.CurrentTitle, SV.Comment);
	}

	playRandomVoice(SV.p_rnd_vlst);

	VOICEFILE_PREFIX = VC_isZa ? VOICEFILE_PREFIX_ZA : VOICEFILE_PREFIX_ED6;
	SV.status.startup = true;
	return true;
}

bool SoraVoice::End() {
	if (!SV.status.startup) return false;
	SV.status.startup = false;
	SV.status.ended = true;

	Player::End();
	Draw::End();
	delete VC_keys; VC_keys = nullptr;
	delete VC_aup; VC_aup = nullptr;

	return true;
}

