#pragma once

#include <string>
#include <map>

#ifndef SVCALL
#define SVCALL __stdcall
#endif

extern std::string globalScene;
extern std::map<std::string, std::string> globalSceneLines;

namespace SoraVoice
{
	void Play(const char* v);
	void Stop();
	void Input();
	void Show(void* pD3DD);

	bool Init();
	bool End();
};


