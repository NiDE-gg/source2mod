/**
 * SourceMod (Source2 / CS2 port)
 * Copyright (C) 2004-2026 AlliedModders LLC. All rights reserved.
 * Licensed under the GNU GPL, version 3, with the SourceMod
 * linking exception. See LICENSE.txt for details.
 */

#include "sourcemod.h"
#include "sourcemm_api.h"
#include "sm_autonatives.h"
#include <iserver.h>
#include <tier1/random.h>

/* Resolve from the current server on every call: the globals belong to a
 * level and Metamod's Source1 GetCGlobals() path is null on Source2. */
static CGlobalVars *CurrentGlobals(IPluginContext *context)
{
	auto *server = netservice ? netservice->GetIGameServer() : nullptr;
	auto *globals = server ? server->GetGlobals() : nullptr;
	if (!globals)
		context->ThrowNativeError("Game globals are unavailable (no active game server)");
	return globals;
}

static cell_t CopyName(IPluginContext *context, const cell_t *params, const char *name)
{
	if (params[2] < 0)
		return context->ThrowNativeError("Invalid buffer length %d", params[2]);
	size_t written = 0;
	/* The pinned VM's UTF8 copier reads dest[len - 1] when len is zero.
	 * Use the plain copier when no text can be written. */
	int error = (params[2] <= 1 || !name[0])
		? context->StringToLocal(params[1], params[2], "")
		: context->StringToLocalUTF8(params[1], params[2], name, &written);
	if (error != SP_ERROR_NONE)
		return context->ThrowNativeErrorEx(error, "Invalid output buffer");
	return static_cast<cell_t>(written);
}

static cell_t GetCurrentMap(IPluginContext *context, const cell_t *params)
{
	auto *globals = CurrentGlobals(context);
	if (!globals)
		return 0;
	return CopyName(context, params, STRING(globals->mapname));
}

static cell_t GetGameFolderName(IPluginContext *context, const cell_t *params)
{
	return CopyName(context, params, g_SourceMod.GetGameFolderName());
}

static cell_t GetEngineVersion(IPluginContext *context, const cell_t *params)
{
	return g_SMAPI->GetSourceEngineBuild();
}

static cell_t IsDedicatedServer(IPluginContext *context, const cell_t *params)
{
	return engine->IsDedicatedServer();
}

static cell_t GetGameTime(IPluginContext *context, const cell_t *params)
{
	auto *globals = CurrentGlobals(context);
	return globals ? sp_ftoc(globals->curtime) : 0;
}

static cell_t GetGameTickCount(IPluginContext *context, const cell_t *params)
{
	auto *globals = CurrentGlobals(context);
	return globals ? globals->tickcount : 0;
}

static cell_t GetGameFrameTime(IPluginContext *context, const cell_t *params)
{
	auto *globals = CurrentGlobals(context);
	return globals ? sp_ftoc(globals->frametime) : 0;
}

static cell_t SetRandomSeed(IPluginContext *context, const cell_t *params)
{
	::RandomSeed(params[1]);
	return 1;
}

static cell_t GetRandomInt(IPluginContext *context, const cell_t *params)
{
	return ::RandomInt(params[1], params[2]);
}

static cell_t GetRandomFloat(IPluginContext *context, const cell_t *params)
{
	return sp_ftoc(::RandomFloat(sp_ctof(params[1]), sp_ctof(params[2])));
}

/* Resource precaching and client text require separate Source2 paths.
 * Do not expose them until their behavior is implemented and validated.
 * GetEngineTime is already registered by TimerSys.cpp. */
REGISTER_NATIVES(halflifeS2Natives)
{
	{"GetCurrentMap", GetCurrentMap},
	{"GetGameFolderName", GetGameFolderName},
	{"GetEngineVersion", GetEngineVersion},
	{"IsDedicatedServer", IsDedicatedServer},
	{"GetGameTime", GetGameTime},
	{"GetGameTickCount", GetGameTickCount},
	{"GetGameFrameTime", GetGameFrameTime},
	{"SetRandomSeed", SetRandomSeed},
	{"GetRandomInt", GetRandomInt},
	{"GetRandomFloat", GetRandomFloat},
	{nullptr, nullptr}
};
