/**
 * vim: set ts=4 sw=4 tw=99 noet :
 * =============================================================================
 * SourceMod (Source2 / CS2 port)
 * Copyright (C) 2004-2026 AlliedModders LLC.  All rights reserved.
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, AlliedModders LLC gives you permission to link the
 * code of this program (as well as its derivative works) to "Half-Life 2," the
 * "Source Engine," the "SourcePawn JIT," and any Game MODs that run on software
 * by the Valve Corporation.  You must obey the GNU General Public License in
 * all respects for all other code used.  Additionally, AlliedModders LLC grants
 * this exception to all derivative works.  AlliedModders LLC defines further
 * exceptions, found in LICENSE.txt (as of this writing, version JULY-31-2007),
 * or <http://www.sourcemod.net/license.php>.
 */

/**
 * @file Minimal Source2 console natives.
 *
 * ConCmdManager is not ported yet (it depends on PlayerManager, ChatTriggers
 * and HalfLife2, all disabled under SOURCE2_WIP). This module provides an
 * explicitly scoped subset so that server plugins can register console
 * commands through the already-ported CoreProviderImpl::DefineCommand path
 * (ConCommand registration + SourceHook on ICvar::DispatchConCommand):
 *
 *   - RegServerCmd (the `flags` argument is accepted but IGNORED: FCVAR
 *     semantics differ on Source2 and are not mapped yet)
 *   - GetCmdArgs / GetCmdArg / GetCmdArgString
 *
 * NOT provided here (reported, not faked): RegConsoleCmd, RegAdminCmd,
 * client command dispatch, command listeners, ReplyToCommand sources.
 */

#ifdef SOURCE2_WIP

#include <map>
#include <set>
#include <string>
#include <vector>

/* interfaces.h (cs2 SDK) declares and references a global ::IScriptManager
 * without qualification; it must be parsed before sm_globals.h brings in
 * `using namespace SourceMod`, or the name becomes ambiguous with
 * SourceMod::IScriptManager. */
#include <interfaces/interfaces.h>

#include "sourcemod.h"
#include "sourcemm_api.h"
#include "sm_autonatives.h"
#include "sm_globals.h"
#include "provider.h"
#include "logic_bridge.h"
#include <IPluginSys.h>
#include <IRootConsoleMenu.h>
#include <IAdminSystem.h>
#include <IPlayerHelpers.h>
#include "PlayerManagerS2.h"
/* Must come after the SDK's interfaces.h (pulled in by provider.h): both
 * declare an IScriptManager and the reverse order makes the name ambiguous. */
#include <bridge/include/IScriptManager.h>

using SourceMod::ICommandArgs;
using SourceMod::IPlugin;
using SourceMod::IPluginsListener;

enum class S2CmdType
{
	Server,   /* RegServerCmd: Action (int args) */
	Console,  /* RegConsoleCmd: Action (int client, int args) */
	Admin,    /* RegAdminCmd: console + admin access check */
};

struct S2ServerCmdInfo
{
	IPluginFunction *callback;
	IPluginContext *owner;
	S2CmdType type;
	SourceMod::FlagBits adminflags;
};

/* Command name -> plugin callback. The underlying ConCommands remain owned
 * by CoreProviderImpl (commands_) until shutdown; only the binding to the
 * plugin is removed when the plugin unloads. */
static std::map<std::string, S2ServerCmdInfo> s_server_cmds;
/* Names already handed to DefineCommand: the engine ConCommand lives until
 * shutdown and must never be re-created (that would duplicate it in the
 * engine registry), even if the plugin binding was removed in between
 * (plugin reload). */
static std::set<std::string> s_defined_names;
static std::vector<const ICommandArgs *> s_cmd_args_stack;

static bool DispatchS2ServerCommand(const std::string &name, int slot, const ICommandArgs *args)
{
	auto it = s_server_cmds.find(name);
	if (it == s_server_cmds.end() || it->second.callback == nullptr)
		return false;

	S2ServerCmdInfo &info = it->second;

	/* GameHooks hands us the raw player slot; the server console is -1,
	 * which maps to SourceMod client 0. */
	int client = (slot < 0) ? 0 : slot + 1;

	if (info.type != S2CmdType::Server && client != 0)
	{
		SourceMod::IGamePlayer *pPlayer = g_PlayersS2.GetGamePlayer(client);
		if (pPlayer == nullptr || !pPlayer->IsConnected())
			return false;

		if (info.type == S2CmdType::Admin &&
		    (adminsys == nullptr || !adminsys->CheckAccess(client, name.c_str(), info.adminflags, false)))
		{
			pPlayer->PrintToConsole("[SM] You do not have access to this command.\n");
			return true;
		}
	}

	s_cmd_args_stack.push_back(args);

	IPluginFunction *fn = info.callback;
	cell_t result = 0;
	if (info.type != S2CmdType::Server)
		fn->PushCell(client);
	fn->PushCell(args->ArgC() - 1);
	fn->Execute(&result);

	s_cmd_args_stack.pop_back();

	/* The registered ConCommand has no useful engine callback:
	 * always supersede the original dispatch. */
	return true;
}

static cell_t RegisterS2Command(IPluginContext *pContext, const cell_t *params,
                                S2CmdType type, SourceMod::FlagBits adminflags)
{
	char *name, *help;
	pContext->LocalToString(params[1], &name);
	pContext->LocalToString(params[3], &help);

	if (!name[0])
		return pContext->ThrowNativeError("Command name must not be empty");

	IPluginFunction *pFunction = pContext->GetFunctionById(params[2]);
	if (!pFunction)
		return pContext->ThrowNativeError("Invalid function id (%X)", params[2]);

	std::string key(name);
	s_server_cmds[key] = S2ServerCmdInfo{pFunction, pContext, type, adminflags};

	if (s_defined_names.insert(key).second)
	{
		/* SECURITY: never shadow a command that already exists in the
		 * engine registry. The client-execute flag below would otherwise
		 * be applied to the engine's own command (e.g. "quit"), letting
		 * any client run it. Hooking existing engine commands is future
		 * work (full ConCmdManager port). */
		if (icvar->FindConCommand(name, true).IsValidRef())
		{
			s_defined_names.erase(key);
			s_server_cmds.erase(key);
			return pContext->ThrowNativeError(
				"Command \"%s\" already exists in the engine; hooking existing commands is not supported yet", name);
		}

		sCoreProviderImpl.DefineCommand(name, help,
			[key](int client, const ICommandArgs *args) -> bool {
				return DispatchS2ServerCommand(key, client, args);
			});
	}

	/* Keep the engine flag in sync with the registered type on every
	 * (re-)registration: a stale FCVAR_CLIENT_CAN_EXECUTE from a previous
	 * registration must not survive a type change to Server. The command
	 * is guaranteed to be ours (shadowing is refused above). The engine
	 * refuses client->server dispatch without this flag (verified live). */
	{
		ConCommandRef ref = icvar->FindConCommand(name, true);
		if (ref.IsValidRef())
		{
			if (type != S2CmdType::Server)
				ref.AddFlags(FCVAR_CLIENT_CAN_EXECUTE);
			else
				ref.RemoveFlags(FCVAR_CLIENT_CAN_EXECUTE);
		}
	}

	return 1;
}

class S2ConsoleNatives :
	public SMGlobalClass,
	public IPluginsListener
{
public:
	void OnSourceModAllInitialized() override
	{
		scripts->AddPluginsListener(this);
	}

	void OnSourceModShutdown() override
	{
		s_server_cmds.clear();
	}

	void OnPluginDestroyed(IPlugin *plugin) override
	{
		IPluginContext *ctx = plugin->GetBaseContext();
		for (auto it = s_server_cmds.begin(); it != s_server_cmds.end(); )
		{
			if (it->second.owner == ctx)
				it = s_server_cmds.erase(it);
			else
				++it;
		}
	}
} s_S2ConsoleNatives;

/* FCVAR-style flags params are accepted but ignored: see the file header. */

static cell_t sm_RegServerCmd(IPluginContext *pContext, const cell_t *params)
{
	return RegisterS2Command(pContext, params, S2CmdType::Server, 0);
}

static cell_t sm_RegConsoleCmd(IPluginContext *pContext, const cell_t *params)
{
	return RegisterS2Command(pContext, params, S2CmdType::Console, 0);
}

/* RegAdminCmd(cmd, callback, adminflags, description="", group="", flags=0) */
static cell_t sm_RegAdminCmd(IPluginContext *pContext, const cell_t *params)
{
	char *name, *help;
	pContext->LocalToString(params[1], &name);
	pContext->LocalToString(params[4], &help);

	if (!name[0])
		return pContext->ThrowNativeError("Command name must not be empty");

	IPluginFunction *pFunction = pContext->GetFunctionById(params[2]);
	if (!pFunction)
		return pContext->ThrowNativeError("Invalid function id (%X)", params[2]);

	std::string key(name);
	const S2CmdType type = S2CmdType::Admin;
	s_server_cmds[key] = S2ServerCmdInfo{pFunction, pContext, type,
	                                     (SourceMod::FlagBits)params[3]};

	if (s_defined_names.insert(key).second)
	{
		/* SECURITY: never shadow a command that already exists in the
		 * engine registry. The client-execute flag below would otherwise
		 * be applied to the engine's own command (e.g. "quit"), letting
		 * any client run it. Hooking existing engine commands is future
		 * work (full ConCmdManager port). */
		if (icvar->FindConCommand(name, true).IsValidRef())
		{
			s_defined_names.erase(key);
			s_server_cmds.erase(key);
			return pContext->ThrowNativeError(
				"Command \"%s\" already exists in the engine; hooking existing commands is not supported yet", name);
		}

		sCoreProviderImpl.DefineCommand(name, help,
			[key](int client, const ICommandArgs *args) -> bool {
				return DispatchS2ServerCommand(key, client, args);
			});
	}

	/* Keep the engine flag in sync with the registered type on every
	 * (re-)registration: a stale FCVAR_CLIENT_CAN_EXECUTE from a previous
	 * registration must not survive a type change to Server. The command
	 * is guaranteed to be ours (shadowing is refused above). The engine
	 * refuses client->server dispatch without this flag (verified live). */
	{
		ConCommandRef ref = icvar->FindConCommand(name, true);
		if (ref.IsValidRef())
		{
			if (type != S2CmdType::Server)
				ref.AddFlags(FCVAR_CLIENT_CAN_EXECUTE);
			else
				ref.RemoveFlags(FCVAR_CLIENT_CAN_EXECUTE);
		}
	}

	return 1;
}

static const ICommandArgs *PeekS2CommandArgs(IPluginContext *pContext)
{
	if (s_cmd_args_stack.empty())
		return nullptr;
	return s_cmd_args_stack.back();
}

static cell_t sm_GetCmdArgs(IPluginContext *pContext, const cell_t *params)
{
	const ICommandArgs *pCmd = PeekS2CommandArgs(pContext);
	if (!pCmd)
		return pContext->ThrowNativeError("No command callback available");

	return pCmd->ArgC() - 1;
}

static cell_t sm_GetCmdArg(IPluginContext *pContext, const cell_t *params)
{
	const ICommandArgs *pCmd = PeekS2CommandArgs(pContext);
	if (!pCmd)
		return pContext->ThrowNativeError("No command callback available");

	const char *arg = pCmd->Arg(params[1]);

	size_t length;
	pContext->StringToLocalUTF8(params[2], params[3], arg ? arg : "", &length);

	return (cell_t)length;
}

static cell_t sm_GetCmdArgString(IPluginContext *pContext, const cell_t *params)
{
	const ICommandArgs *pCmd = PeekS2CommandArgs(pContext);
	if (!pCmd)
		return pContext->ThrowNativeError("No command callback available");

	const char *args = pCmd->ArgS();

	size_t length;
	pContext->StringToLocalUTF8(params[1], params[2], args ? args : "", &length);

	return (cell_t)length;
}

static cell_t sm_GetEngineTime(IPluginContext *pContext, const cell_t *params)
{
	/* Same semantics as the Source1 native on modern engines:
	 * high-precision engine clock (Plat_FloatTime). */
	return sp_ftoc((float)Plat_FloatTime());
}

REGISTER_NATIVES(s2ConsoleNatives)
{
	{"GetEngineTime",		sm_GetEngineTime},
	{"RegServerCmd",		sm_RegServerCmd},
	{"RegConsoleCmd",		sm_RegConsoleCmd},
	{"RegAdminCmd",			sm_RegAdminCmd},
	{"GetCmdArgs",			sm_GetCmdArgs},
	{"GetCmdArg",			sm_GetCmdArg},
	{"GetCmdArgString",		sm_GetCmdArgString},
	{NULL,					NULL}
};

#endif // SOURCE2_WIP
