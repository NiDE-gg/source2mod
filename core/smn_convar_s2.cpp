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
 * @file Source2 ConVar manager and natives.
 *
 * The Source1 ConVarManager/smn_console pair is built on the old ConVar
 * object model. On Source2 convars live in an engine-side registry accessed
 * through ConVarRefAbstract, and plugin-created convars are typed; SourceMod
 * creates them as string-typed (CConVar<CUtlString>), matching Metamod's own
 * approach, and keeps SM-style float bounds on its side (clamped from the
 * global change callback).
 *
 * Provided: CreateConVar, FindConVar, Hook/UnhookConVarChange, typed
 * getters/setters, name/description/default/bounds/flags, ResetConVar, and
 * the ConVar methodmap aliases.
 *
 * NOT provided (reported, not faked): QueryClientConVar, SendConVarValue,
 * per-client replicated value overrides, ConVar.Plugin.get, and
 * SetConVarBounds on engine-owned convars. Only FCVAR bits whose meaning is
 * unchanged between the Source1 includes and Source2 are passed through.
 */

#ifdef SOURCE2_WIP

#include <map>
#include <string>

#include "sourcemod.h"
#include "sourcemm_api.h"
#include "sm_autonatives.h"
#include "sm_globals.h"
#include "sm_stringutil.h"
#include "logic_bridge.h"
#include <IHandleSys.h>
#include <IForwardSys.h>
#include <am-string.h>

/* FCVAR bits shared verbatim between the SourceMod includes (Source1
 * numbering) and the Source2 SDK (verified against tier1/convar.h):
 * DEVELOPMENTONLY(1), HIDDEN(4), PROTECTED(5), SPONLY(6), ARCHIVE(7),
 * NOTIFY(8), UNLOGGED(11), REPLICATED(13), CHEAT(14). Everything else is
 * dropped: the remaining bits changed meaning on Source2. */
static const uint64 k_PassthroughCvarFlags =
	FCVAR_DEVELOPMENTONLY | FCVAR_HIDDEN | FCVAR_PROTECTED | FCVAR_SPONLY |
	FCVAR_ARCHIVE | FCVAR_NOTIFY | FCVAR_UNLOGGED | FCVAR_REPLICATED |
	FCVAR_CHEAT;

struct S2ConVarInfo
{
	Handle_t handle = BAD_HANDLE;
	bool sourceMod = false;              /* created by a plugin through us */
	IChangeableForward *pChangeForward = nullptr;
	ConVarRefAbstract ref;               /* engine registry reference */
	CConVar<CUtlString> *owned = nullptr;/* backing object when sourceMod */
	bool hasMin = false, hasMax = false; /* SM-side bounds (sourceMod only) */
	float minVal = 0.0f, maxVal = 0.0f;

	S2ConVarInfo() : ref(ConVarRef()) {}
};

static const ParamType CONVARCHANGE_PARAMS[] = {Param_Cell, Param_String, Param_String};

class ConVarManagerS2 :
	public SMGlobalClass,
	public IHandleTypeDispatch
{
public:
	HandleType_t m_ConVarType = 0;
	std::map<std::string, S2ConVarInfo *> m_ByName;    /* lowercase name */
	std::map<uint16, S2ConVarInfo *> m_ByAccessIndex;  /* change dispatch */
	bool m_bClamping = false;

	void OnSourceModStartup(bool late) override
	{
		HandleAccess sec;
		handlesys->InitAccessDefaults(nullptr, &sec);
		sec.access[HandleAccess_Read] = 0;
		sec.access[HandleAccess_Delete] = HANDLE_RESTRICT_IDENTITY | HANDLE_RESTRICT_OWNER;
		sec.access[HandleAccess_Clone] = HANDLE_RESTRICT_IDENTITY | HANDLE_RESTRICT_OWNER;
		m_ConVarType = handlesys->CreateType("ConVar", this, 0, NULL, &sec, g_pCoreIdent, NULL);
	}

	void OnSourceModAllInitialized() override
	{
		icvar->InstallGlobalChangeCallback(OnGlobalCvarChange);
	}

	void OnSourceModShutdown() override
	{
		icvar->RemoveGlobalChangeCallback(OnGlobalCvarChange);

		HandleSecurity sec(NULL, g_pCoreIdent);
		for (auto &it : m_ByName)
		{
			S2ConVarInfo *pInfo = it.second;
			if (pInfo->pChangeForward != nullptr)
				forwardsys->ReleaseForward(pInfo->pChangeForward);
			handlesys->FreeHandle(pInfo->handle, &sec);
			delete pInfo->owned;
			delete pInfo;
		}
		m_ByName.clear();
		m_ByAccessIndex.clear();
		handlesys->RemoveType(m_ConVarType, g_pCoreIdent);
	}

	void OnHandleDestroy(HandleType_t type, void *object) override
	{
		/* ConVar handles share the S2ConVarInfo owned by the manager;
		 * everything is reclaimed at shutdown. */
	}

	S2ConVarInfo *FindByName(const char *name)
	{
		auto it = m_ByName.find(ke::Lowercase(name));
		return (it == m_ByName.end()) ? nullptr : it->second;
	}

	S2ConVarInfo *Track(S2ConVarInfo *pInfo, const char *name)
	{
		m_ByName[ke::Lowercase(name)] = pInfo;
		m_ByAccessIndex[pInfo->ref.GetAccessIndex()] = pInfo;
		return pInfo;
	}

	/* Wrap an engine-registered convar into a handle (no ownership). */
	Handle_t WrapExisting(ConVarRef engineRef)
	{
		S2ConVarInfo *pInfo = new S2ConVarInfo();
		pInfo->ref = ConVarRefAbstract(engineRef);
		pInfo->sourceMod = false;

		Handle_t hndl = handlesys->CreateHandle(m_ConVarType, pInfo, NULL, g_pCoreIdent, NULL);
		if (hndl == BAD_HANDLE)
		{
			delete pInfo;
			return BAD_HANDLE;
		}
		pInfo->handle = hndl;
		Track(pInfo, pInfo->ref.GetName());
		return hndl;
	}

	Handle_t CreateConVar(IPluginContext *pContext, const char *name, const char *defaultVal,
	                      const char *description, int flags,
	                      bool hasMin, float min, bool hasMax, float max)
	{
		if (S2ConVarInfo *pInfo = FindByName(name))
			return pInfo->handle;

		ConVarRef existing = icvar->FindConVar(name, true);
		if (existing.IsValidRef())
			return WrapExisting(existing);

		/* Same-name console command check (mirrors Source1 behavior). */
		if (icvar->FindConCommand(name, true).IsValidRef())
			return BAD_HANDLE;

		uint64 s2flags = ((uint64)(uint32)flags) & k_PassthroughCvarFlags;

		S2ConVarInfo *pInfo = new S2ConVarInfo();
		pInfo->sourceMod = true;
		pInfo->hasMin = hasMin;
		pInfo->minVal = min;
		pInfo->hasMax = hasMax;
		pInfo->maxVal = max;

		Handle_t hndl = handlesys->CreateHandle(m_ConVarType, pInfo, NULL, g_pCoreIdent, NULL);
		if (hndl == BAD_HANDLE)
		{
			delete pInfo;
			return BAD_HANDLE;
		}
		pInfo->handle = hndl;

		/* Plugin convars are string-typed on Source2; the registry copies
		 * name/help, but CConVar keeps the name pointer in its creation
		 * info, so duplicate them with the manager's lifetime. */
		pInfo->owned = new CConVar<CUtlString>(sm_strdup(name), s2flags,
		                                       sm_strdup(description),
		                                       CUtlString(defaultVal));
		pInfo->ref = *pInfo->owned;

		Track(pInfo, name);
		return hndl;
	}

	Handle_t FindConVar(const char *name)
	{
		if (S2ConVarInfo *pInfo = FindByName(name))
			return pInfo->handle;

		ConVarRef existing = icvar->FindConVar(name, true);
		if (!existing.IsValidRef())
			return BAD_HANDLE;
		return WrapExisting(existing);
	}

	void HookChange(S2ConVarInfo *pInfo, IPluginFunction *pFunction)
	{
		if (pInfo->pChangeForward == nullptr)
			pInfo->pChangeForward = forwardsys->CreateForwardEx(NULL, ET_Ignore, 3, CONVARCHANGE_PARAMS);
		pInfo->pChangeForward->AddFunction(pFunction);
	}

	bool UnhookChange(S2ConVarInfo *pInfo, IPluginFunction *pFunction)
	{
		if (pInfo->pChangeForward == nullptr)
			return false;
		return pInfo->pChangeForward->RemoveFunction(pFunction);
	}

	static void OnGlobalCvarChange(ConVarRefAbstract *ref, CSplitScreenSlot nSlot,
	                               const char *pNewValue, const char *pOldValue,
	                               ConVarUserInfoSet_t *userinfo_data);
};

static ConVarManagerS2 s_ConVarManager;

void ConVarManagerS2::OnGlobalCvarChange(ConVarRefAbstract *ref, CSplitScreenSlot nSlot,
                                         const char *pNewValue, const char *pOldValue,
                                         ConVarUserInfoSet_t *userinfo_data)
{
	auto it = s_ConVarManager.m_ByAccessIndex.find(ref->GetAccessIndex());
	if (it == s_ConVarManager.m_ByAccessIndex.end())
		return;
	S2ConVarInfo *pInfo = it->second;

	/* SM-side bound enforcement for plugin convars (the engine does not
	 * clamp string-typed cvars). Re-setting fires this callback again with
	 * an in-bounds value, so recursion stops; m_bClamping skips the
	 * intermediate out-of-bounds notification. */
	if (pInfo->sourceMod && (pInfo->hasMin || pInfo->hasMax) && !s_ConVarManager.m_bClamping)
	{
		float val = (float)atof(pNewValue);
		float clamped = val;
		if (pInfo->hasMin && clamped < pInfo->minVal)
			clamped = pInfo->minVal;
		if (pInfo->hasMax && clamped > pInfo->maxVal)
			clamped = pInfo->maxVal;
		if (clamped != val)
		{
			char buffer[64];
			ke::SafeSprintf(buffer, sizeof(buffer), "%g", clamped);
			s_ConVarManager.m_bClamping = true;
			/* Per the SDK note in convar.h, inside a change callback the set
			 * must go through a static_cast of the received ref (the
			 * CConVarRef constructor cast is invalid once the set is
			 * queued and crashes). Plugin cvars are string-typed. */
			if (ref->GetType() == EConVarType_String)
				static_cast<CConVarRef<CUtlString> *>(ref)->Set(CUtlString(buffer));
			s_ConVarManager.m_bClamping = false;
			return;
		}
	}

	if (pInfo->pChangeForward != nullptr && pInfo->pChangeForward->GetFunctionCount() > 0)
	{
		pInfo->pChangeForward->PushCell(pInfo->handle);
		pInfo->pChangeForward->PushString(pOldValue ? pOldValue : "");
		pInfo->pChangeForward->PushString(pNewValue ? pNewValue : "");
		pInfo->pChangeForward->Execute(NULL);
	}
}

static S2ConVarInfo *ReadConVarHandle(IPluginContext *pContext, cell_t handleCell)
{
	Handle_t hndl = (Handle_t)handleCell;
	HandleSecurity sec(NULL, g_pCoreIdent);
	S2ConVarInfo *pInfo = nullptr;

	HandleError err = handlesys->ReadHandle(hndl, s_ConVarManager.m_ConVarType, &sec, (void **)&pInfo);
	if (err != HandleError_None)
	{
		pContext->ThrowNativeError("Invalid convar handle %x (error %d)", hndl, err);
		return nullptr;
	}
	return pInfo;
}

/* Natives */

static cell_t sm_CreateConVar(IPluginContext *pContext, const cell_t *params)
{
	char *name, *defaultVal, *description;
	pContext->LocalToString(params[1], &name);

	if (!name[0] || strchr(name, ' ') != nullptr)
		return pContext->ThrowNativeError("Convar with invalid name (%s)", name);

	pContext->LocalToString(params[2], &defaultVal);
	pContext->LocalToString(params[3], &description);

	bool hasMin = params[5] != 0;
	bool hasMax = params[7] != 0;
	float min = sp_ctof(params[6]);
	float max = sp_ctof(params[8]);

	return s_ConVarManager.CreateConVar(pContext, name, defaultVal, description,
	                                    params[4], hasMin, min, hasMax, max);
}

static cell_t sm_FindConVar(IPluginContext *pContext, const cell_t *params)
{
	char *name;
	pContext->LocalToString(params[1], &name);
	return s_ConVarManager.FindConVar(name);
}

static cell_t sm_HookConVarChange(IPluginContext *pContext, const cell_t *params)
{
	S2ConVarInfo *pInfo = ReadConVarHandle(pContext, params[1]);
	if (pInfo == nullptr)
		return 0;

	IPluginFunction *pFunction = pContext->GetFunctionById(params[2]);
	if (pFunction == nullptr)
		return pContext->ThrowNativeError("Invalid function id (%X)", params[2]);

	s_ConVarManager.HookChange(pInfo, pFunction);
	return 1;
}

static cell_t sm_UnhookConVarChange(IPluginContext *pContext, const cell_t *params)
{
	S2ConVarInfo *pInfo = ReadConVarHandle(pContext, params[1]);
	if (pInfo == nullptr)
		return 0;

	IPluginFunction *pFunction = pContext->GetFunctionById(params[2]);
	if (pFunction == nullptr)
		return pContext->ThrowNativeError("Invalid function id (%X)", params[2]);

	if (!s_ConVarManager.UnhookChange(pInfo, pFunction))
		return pContext->ThrowNativeError("Invalid hook callback specified for convar \"%s\"", pInfo->ref.GetName());
	return 1;
}

static cell_t sm_GetConVarBool(IPluginContext *pContext, const cell_t *params)
{
	S2ConVarInfo *pInfo = ReadConVarHandle(pContext, params[1]);
	return (pInfo != nullptr) ? pInfo->ref.GetBool() : 0;
}

static cell_t sm_GetConVarInt(IPluginContext *pContext, const cell_t *params)
{
	S2ConVarInfo *pInfo = ReadConVarHandle(pContext, params[1]);
	if (pInfo == nullptr)
		return 0;
	/* String-typed cvars (all plugin cvars): the SDK string->int conversion
	 * rejects float-formatted text ("1.5"), so go through float like
	 * Source1 did (atoi semantics on floats truncate). */
	if (pInfo->ref.GetType() == EConVarType_String)
		return (cell_t)pInfo->ref.GetFloat();
	return pInfo->ref.GetInt();
}

static cell_t sm_GetConVarFloat(IPluginContext *pContext, const cell_t *params)
{
	S2ConVarInfo *pInfo = ReadConVarHandle(pContext, params[1]);
	return (pInfo != nullptr) ? sp_ftoc(pInfo->ref.GetFloat()) : 0;
}

static cell_t sm_GetConVarString(IPluginContext *pContext, const cell_t *params)
{
	S2ConVarInfo *pInfo = ReadConVarHandle(pContext, params[1]);
	if (pInfo == nullptr)
		return 0;

	CUtlString value = pInfo->ref.GetString();
	size_t length;
	pContext->StringToLocalUTF8(params[2], params[3], value.Get() ? value.Get() : "", &length);
	return (cell_t)length;
}

static cell_t sm_SetConVarNum(IPluginContext *pContext, const cell_t *params)
{
	S2ConVarInfo *pInfo = ReadConVarHandle(pContext, params[1]);
	if (pInfo == nullptr)
		return 0;

	char buffer[32];
	ke::SafeSprintf(buffer, sizeof(buffer), "%d", params[2]);
	pInfo->ref.SetString(CUtlString(buffer));
	return 1;
}

static cell_t sm_SetConVarFloat(IPluginContext *pContext, const cell_t *params)
{
	S2ConVarInfo *pInfo = ReadConVarHandle(pContext, params[1]);
	if (pInfo == nullptr)
		return 0;

	char buffer[64];
	ke::SafeSprintf(buffer, sizeof(buffer), "%f", sp_ctof(params[2]));
	pInfo->ref.SetString(CUtlString(buffer));
	return 1;
}

static cell_t sm_SetConVarString(IPluginContext *pContext, const cell_t *params)
{
	S2ConVarInfo *pInfo = ReadConVarHandle(pContext, params[1]);
	if (pInfo == nullptr)
		return 0;

	char *value;
	pContext->LocalToString(params[2], &value);
	pInfo->ref.SetString(CUtlString(value));
	return 1;
}

static cell_t sm_ResetConVar(IPluginContext *pContext, const cell_t *params)
{
	S2ConVarInfo *pInfo = ReadConVarHandle(pContext, params[1]);
	if (pInfo == nullptr)
		return 0;
	pInfo->ref.Revert();
	return 1;
}

static cell_t sm_GetConVarName(IPluginContext *pContext, const cell_t *params)
{
	S2ConVarInfo *pInfo = ReadConVarHandle(pContext, params[1]);
	if (pInfo == nullptr)
		return 0;
	size_t length;
	pContext->StringToLocalUTF8(params[2], params[3], pInfo->ref.GetName(), &length);
	return (cell_t)length;
}

static cell_t sm_GetConVarDescription(IPluginContext *pContext, const cell_t *params)
{
	S2ConVarInfo *pInfo = ReadConVarHandle(pContext, params[1]);
	if (pInfo == nullptr)
		return 0;
	size_t length;
	const char *help = pInfo->ref.HasHelpText() ? pInfo->ref.GetHelpText() : "";
	pContext->StringToLocalUTF8(params[2], params[3], help, &length);
	return (cell_t)length;
}

static cell_t GetConVarDefault(IPluginContext *pContext, const cell_t *params)
{
	S2ConVarInfo *pInfo = ReadConVarHandle(pContext, params[1]);
	if (pInfo == nullptr)
		return 0;

	CBufferString buf;
	pInfo->ref.GetDefaultAsString(buf);
	size_t length;
	pContext->StringToLocalUTF8(params[2], params[3], buf.Get(), &length);
	return (cell_t)length;
}

static cell_t sm_GetConVarFlags(IPluginContext *pContext, const cell_t *params)
{
	S2ConVarInfo *pInfo = ReadConVarHandle(pContext, params[1]);
	if (pInfo == nullptr)
		return 0;
	/* Only the low, meaning-stable bits are representable in the plugin's
	 * 32-bit view. */
	return (cell_t)(pInfo->ref.GetFlags() & 0xFFFFFFFFull);
}

static cell_t sm_SetConVarFlags(IPluginContext *pContext, const cell_t *params)
{
	S2ConVarInfo *pInfo = ReadConVarHandle(pContext, params[1]);
	if (pInfo == nullptr)
		return 0;

	uint64 newflags = ((uint64)(uint32)params[2]) & k_PassthroughCvarFlags;
	pInfo->ref.RemoveFlags(k_PassthroughCvarFlags);
	pInfo->ref.AddFlags(newflags);
	return 1;
}

/* GetConVarBounds(Handle, ConVarBounds:type, &Float:value) -> bool */
static cell_t sm_GetConVarBounds(IPluginContext *pContext, const cell_t *params)
{
	S2ConVarInfo *pInfo = ReadConVarHandle(pContext, params[1]);
	if (pInfo == nullptr)
		return 0;

	cell_t *addr;
	pContext->LocalToPhysAddr(params[3], &addr);

	bool wantMax = (params[2] == 0); /* ConVarBound_Upper = 0, Lower = 1 */
	bool has;
	float value;

	if (pInfo->sourceMod)
	{
		has = wantMax ? pInfo->hasMax : pInfo->hasMin;
		value = wantMax ? pInfo->maxVal : pInfo->minVal;
	}
	else
	{
		has = wantMax ? pInfo->ref.HasMax() : pInfo->ref.HasMin();
		value = 0.0f;
		if (has)
		{
			CBufferString buf;
			if (wantMax)
				pInfo->ref.GetMaxAsString(buf);
			else
				pInfo->ref.GetMinAsString(buf);
			value = (float)atof(buf.Get());
		}
	}

	*addr = sp_ftoc(has ? value : 0.0f);
	return has ? 1 : 0;
}

/* SetConVarBounds(Handle, ConVarBounds:type, bool:set, Float:value) */
static cell_t sm_SetConVarBounds(IPluginContext *pContext, const cell_t *params)
{
	S2ConVarInfo *pInfo = ReadConVarHandle(pContext, params[1]);
	if (pInfo == nullptr)
		return 0;

	if (!pInfo->sourceMod)
	{
		return pContext->ThrowNativeError(
			"SetConVarBounds is not supported on engine-owned convars on Source2 yet (convar \"%s\")",
			pInfo->ref.GetName());
	}

	bool wantMax = (params[2] == 0); /* ConVarBound_Upper = 0, Lower = 1 */
	bool set = (params[3] != 0);
	float value = sp_ctof(params[4]);

	if (wantMax)
	{
		pInfo->hasMax = set;
		pInfo->maxVal = value;
	}
	else
	{
		pInfo->hasMin = set;
		pInfo->minVal = value;
	}
	return 1;
}

REGISTER_NATIVES(s2ConVarNatives)
{
	{"CreateConVar",			sm_CreateConVar},
	{"FindConVar",				sm_FindConVar},
	{"HookConVarChange",		sm_HookConVarChange},
	{"UnhookConVarChange",		sm_UnhookConVarChange},
	{"GetConVarBool",			sm_GetConVarBool},
	{"SetConVarBool",			sm_SetConVarNum},
	{"GetConVarInt",			sm_GetConVarInt},
	{"SetConVarInt",			sm_SetConVarNum},
	{"GetConVarFloat",			sm_GetConVarFloat},
	{"SetConVarFloat",			sm_SetConVarFloat},
	{"GetConVarString",			sm_GetConVarString},
	{"SetConVarString",			sm_SetConVarString},
	{"ResetConVar",				sm_ResetConVar},
	{"GetConVarName",			sm_GetConVarName},
	{"GetConVarDefault",		GetConVarDefault},
	{"GetConVarFlags",			sm_GetConVarFlags},
	{"SetConVarFlags",			sm_SetConVarFlags},
	{"GetConVarBounds",			sm_GetConVarBounds},
	{"SetConVarBounds",			sm_SetConVarBounds},

	/* ConVar methodmap */
	{"ConVar.BoolValue.get",	sm_GetConVarBool},
	{"ConVar.BoolValue.set",	sm_SetConVarNum},
	{"ConVar.FloatValue.get",	sm_GetConVarFloat},
	{"ConVar.FloatValue.set",	sm_SetConVarFloat},
	{"ConVar.IntValue.get",		sm_GetConVarInt},
	{"ConVar.IntValue.set",		sm_SetConVarNum},
	{"ConVar.Flags.get",		sm_GetConVarFlags},
	{"ConVar.Flags.set",		sm_SetConVarFlags},
	{"ConVar.SetBool",			sm_SetConVarNum},
	{"ConVar.SetInt",			sm_SetConVarNum},
	{"ConVar.SetFloat",			sm_SetConVarFloat},
	{"ConVar.GetString",		sm_GetConVarString},
	{"ConVar.SetString",		sm_SetConVarString},
	{"ConVar.RestoreDefault",	sm_ResetConVar},
	{"ConVar.GetDefault",		GetConVarDefault},
	{"ConVar.GetBounds",		sm_GetConVarBounds},
	{"ConVar.SetBounds",		sm_SetConVarBounds},
	{"ConVar.GetName",			sm_GetConVarName},
	{"ConVar.GetDescription",	sm_GetConVarDescription},
	{"ConVar.AddChangeHook",	sm_HookConVarChange},
	{"ConVar.RemoveChangeHook",	sm_UnhookConVarChange},
	{NULL,						NULL}
};

#endif // SOURCE2_WIP
