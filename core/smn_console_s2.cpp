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

/* interfaces.h (SDK cs2) déclare et référence un ::IScriptManager global sans
 * qualification ; il doit être parsé avant le `using namespace SourceMod` de
 * sm_globals.h, sinon le nom devient ambigu avec SourceMod::IScriptManager. */
#include <interfaces/interfaces.h>

#include "sourcemod.h"
#include "sm_autonatives.h"
#include "sm_globals.h"
#include "provider.h"
#include "logic_bridge.h"
#include <IPluginSys.h>
#include <IRootConsoleMenu.h>
/* Doit venir après interfaces.h du SDK (tiré par provider.h) : les deux
 * déclarent un IScriptManager et l'ordre inverse rend le nom ambigu. */
#include <bridge/include/IScriptManager.h>

using SourceMod::ICommandArgs;
using SourceMod::IPlugin;
using SourceMod::IPluginsListener;

struct S2ServerCmdInfo
{
	IPluginFunction *callback;
	IPluginContext *owner;
};

/* Nom de commande -> callback plugin. Les ConCommand sous-jacentes restent
 * possédées par CoreProviderImpl (commands_) jusqu'au shutdown ; seule la
 * liaison vers le plugin est retirée quand celui-ci se décharge. */
static std::map<std::string, S2ServerCmdInfo> s_server_cmds;
/* Noms déjà passés à DefineCommand : la ConCommand moteur vit jusqu'au
 * shutdown, on ne doit jamais la recréer (sinon doublon côté registre engine),
 * même si la liaison plugin a été retirée entre-temps (reload). */
static std::set<std::string> s_defined_names;
static std::vector<const ICommandArgs *> s_cmd_args_stack;

static bool DispatchS2ServerCommand(const std::string &name, int client, const ICommandArgs *args)
{
	auto it = s_server_cmds.find(name);
	if (it == s_server_cmds.end() || it->second.callback == nullptr)
		return false;

	s_cmd_args_stack.push_back(args);

	IPluginFunction *fn = it->second.callback;
	cell_t result = 0;
	fn->PushCell(args->ArgC() - 1);
	fn->Execute(&result);

	s_cmd_args_stack.pop_back();

	/* La ConCommand enregistrée n'a pas de callback moteur utile :
	 * on supersede toujours le dispatch d'origine. */
	return true;
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

static cell_t sm_RegServerCmd(IPluginContext *pContext, const cell_t *params)
{
	char *name, *help;
	pContext->LocalToString(params[1], &name);
	pContext->LocalToString(params[3], &help);

	if (!name[0])
		return pContext->ThrowNativeError("Command name must not be empty");

	IPluginFunction *pFunction = pContext->GetFunctionById(params[2]);
	if (!pFunction)
		return pContext->ThrowNativeError("Invalid function id (%X)", params[2]);

	/* params[4] (flags) volontairement ignoré : voir l'en-tête du fichier. */

	std::string key(name);
	s_server_cmds[key] = S2ServerCmdInfo{pFunction, pContext};

	if (s_defined_names.insert(key).second)
	{
		sCoreProviderImpl.DefineCommand(name, help,
			[key](int client, const ICommandArgs *args) -> bool {
				return DispatchS2ServerCommand(key, client, args);
			});
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

REGISTER_NATIVES(s2ConsoleNatives)
{
	{"RegServerCmd",		sm_RegServerCmd},
	{"GetCmdArgs",			sm_GetCmdArgs},
	{"GetCmdArg",			sm_GetCmdArg},
	{"GetCmdArgString",		sm_GetCmdArgString},
	{NULL,					NULL}
};

#endif // SOURCE2_WIP
