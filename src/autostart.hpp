#pragma once

#include <string>

struct Loadout;

class AutoStarter {
public:
	static bool LaunchPrograms(const std::string &loadoutName = "");
	static bool LaunchPrograms(const Loadout &loadout);
	static bool QuitPrograms(bool automatic = false);
};
