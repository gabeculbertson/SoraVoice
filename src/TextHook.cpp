#include "TextHook.h"

#include <string>

#using <SoraVoiceLib.dll>

using namespace System;
using namespace std;

void TextHook::HandleText(string game, string text, string voice) {
	SoraVoiceHook::Hook::SendToInterface(gcnew String(game.c_str()), gcnew String(text.c_str()), gcnew String(voice.c_str()));
}