#ifndef LUAVM_HPP // fix redfinition
#define LUAVM_HPP
#include "C:\Users\vzsol\Downloads\roblox-external-source-main(1)\roblox-external-source-main\include\sol\sol.hpp"

namespace silence
{
	class LuaVM
	{
	public:
		sol::state luaState;

		void init();
		void RunScriptSafe(std::string script); // prevent rce and allow to return error type & numbers
	};
}

#endif