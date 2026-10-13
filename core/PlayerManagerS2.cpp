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

#ifdef SOURCE2_WIP

#include "PlayerManagerS2.h"
#include "sourcemod.h"
#include "sourcemm_api.h"
#include "logic_bridge.h"
#include "sm_stringutil.h"

#include <inttypes.h>
#include <am-string.h>

PlayerManagerS2 g_PlayersS2;

/* CS2 caps out at 64 player slots; index 0 is the server console. */
#define S2_MAX_PLAYERS 64
static CPlayerS2 s_Players[S2_MAX_PLAYERS + 1];
static unsigned int s_SerialGen = 0;

SH_DECL_HOOK6(IServerGameClients, ClientConnect, SH_NOATTRIB, 0, bool,
              CPlayerSlot, const char *, uint64, const char *, bool, CBufferString *);
SH_DECL_HOOK6_void(IServerGameClients, OnClientConnected, SH_NOATTRIB, 0,
                   CPlayerSlot, const char *, uint64, const char *, const char *, bool);
SH_DECL_HOOK4_void(IServerGameClients, ClientPutInServer, SH_NOATTRIB, 0,
                   CPlayerSlot, char const *, int, uint64);
SH_DECL_HOOK5_void(IServerGameClients, ClientDisconnect, SH_NOATTRIB, 0,
                   CPlayerSlot, ENetworkDisconnectionReason, const char *, uint64, const char *);
SH_DECL_HOOK1_void(IServerGameClients, ClientSettingsChanged, SH_NOATTRIB, 0, CPlayerSlot);

/*****************
 * CPlayerS2     *
 *****************/

CPlayerS2::CPlayerS2()
{
}

void CPlayerS2::Initialize(int index, const char *name, uint64_t xuid,
                           const char *networkid, const char *address, bool fake)
{
	m_iIndex = index;
	m_bConnected = true;
	m_bInGame = false;
	m_bFakeClient = fake;
	m_bAuthorized = false;
	m_bAdminCheckSignalled = false;
	m_bInKickQueue = false;
	m_Name = name ? name : "";
	m_NetworkId = networkid ? networkid : "";
	m_IpAndPort = address ? address : "";
	m_Ip = m_IpAndPort.substr(0, m_IpAndPort.find(':'));
	m_Xuid = fake ? 0 : xuid;
	m_UserId = engine->GetPlayerUserId(Slot()).Get();
	m_Serial = (++s_SerialGen << 8) | (index & 0xFF);
	m_LangId = 0;
	m_Admin = INVALID_ADMIN_ID;
	m_TempAdmin = false;

	if (m_Xuid != 0)
	{
		char steam2[64];
		unsigned int account = (unsigned int)(m_Xuid & 0xFFFFFFFF);
		ke::SafeSprintf(steam2, sizeof(steam2), "STEAM_1:%u:%u", account & 1, account >> 1);
		m_Steam2Id = steam2;
		ke::SafeSprintf(steam2, sizeof(steam2), "[U:1:%u]", account);
		m_Steam3Id = steam2;
	}
	else
	{
		m_Steam2Id = fake ? "BOT" : "";
		m_Steam3Id = m_Steam2Id;
	}
}

void CPlayerS2::Disconnect()
{
	m_bConnected = false;
	m_bInGame = false;
	m_bAuthorized = false;
	m_bAdminCheckSignalled = false;
	m_bInKickQueue = false;
	m_Serial = 0;
	m_UserId = -1;
	m_Xuid = 0;
	m_Admin = INVALID_ADMIN_ID;
	m_Name.clear();
	m_Steam2Id.clear();
	m_Steam3Id.clear();
	m_NetworkId.clear();
	m_Ip.clear();
	m_IpAndPort.clear();
}

void CPlayerS2::SetName(const char *name)
{
	m_Name = name ? name : "";
}

const char *CPlayerS2::GetName()
{
	return m_Name.c_str();
}

const char *CPlayerS2::GetIPAddress()
{
	return m_Ip.c_str();
}

const char *CPlayerS2::GetAuthString(bool validated)
{
	/* Source2 WIP: Steam ticket validation is not wired up; the XUID the
	 * engine reports at connect time is treated as authoritative. */
	if (!IsAuthorized())
		return nullptr;
	return m_Steam2Id.c_str();
}

edict_t *CPlayerS2::GetEdict()
{
	/* No edicts on Source2. */
	return nullptr;
}

bool CPlayerS2::IsInGame()
{
	return m_bInGame;
}

bool CPlayerS2::IsConnected()
{
	return m_bConnected;
}

bool CPlayerS2::IsFakeClient()
{
	return m_bFakeClient;
}

bool CPlayerS2::IsSourceTV() const
{
	/* CSTV bots are fake clients named by tv_name; not distinguished yet. */
	return false;
}

bool CPlayerS2::IsReplay() const
{
	return false;
}

AdminId CPlayerS2::GetAdminId()
{
	return m_Admin;
}

void CPlayerS2::SetAdminId(AdminId id, bool temp)
{
	if (!m_bConnected)
		return;
	m_Admin = id;
	m_TempAdmin = temp;
}

int CPlayerS2::GetUserId()
{
	return m_UserId;
}

unsigned int CPlayerS2::GetLanguageId()
{
	return m_LangId;
}

void CPlayerS2::SetLanguageId(unsigned int id)
{
	m_LangId = id;
}

IPlayerInfo *CPlayerS2::GetPlayerInfo()
{
	/* IPlayerInfoManager does not exist on Source2. */
	return nullptr;
}

bool CPlayerS2::RunAdminCacheChecks()
{
	AdminId old_id = GetAdminId();
	DoBasicAdminChecks();
	return (GetAdminId() != old_id);
}

void CPlayerS2::DoBasicAdminChecks()
{
	if (GetAdminId() != INVALID_ADMIN_ID || adminsys == nullptr)
		return;

	/* Source2 WIP: unlike the Source1 manager, no admin password checks and
	 * no kick-on-name-match (both need ConVar + timer support). */
	AdminId id;
	if (!m_Name.empty() &&
	    (id = adminsys->FindAdminByIdentity("name", m_Name.c_str())) != INVALID_ADMIN_ID)
	{
		SetAdminId(id, false);
		return;
	}

	if (!m_Ip.empty() &&
	    (id = adminsys->FindAdminByIdentity("ip", m_Ip.c_str())) != INVALID_ADMIN_ID)
	{
		SetAdminId(id, false);
		return;
	}

	if (IsAuthorized() && !m_Steam2Id.empty())
	{
		if ((id = adminsys->FindAdminByIdentity("steam", m_Steam2Id.c_str())) != INVALID_ADMIN_ID)
		{
			SetAdminId(id, false);
		}
	}
}

void CPlayerS2::NotifyPostAdminChecks()
{
	if (m_bAdminCheckSignalled)
		return;
	m_bAdminCheckSignalled = true;
	g_PlayersS2.NotifyPostAdminChecks(this);
}

unsigned int CPlayerS2::GetSerial()
{
	return m_Serial;
}

int CPlayerS2::GetIndex() const
{
	return m_iIndex;
}

bool CPlayerS2::IsAuthorized()
{
	return m_bAuthorized || m_bFakeClient;
}

void CPlayerS2::Kick(const char *message)
{
	MarkAsBeingKicked();
	engine->KickClient(Slot(), message, NETWORK_DISCONNECT_KICKED);
}

bool CPlayerS2::IsInKickQueue()
{
	return m_bInKickQueue;
}

void CPlayerS2::MarkAsBeingKicked()
{
	m_bInKickQueue = true;
}

void CPlayerS2::PrintToConsole(const char *pMsg)
{
	if (!m_bConnected || m_bFakeClient)
		return;
	engine->ClientPrintf(Slot(), pMsg);
}

void CPlayerS2::ClearAdmin()
{
	m_Admin = INVALID_ADMIN_ID;
	m_TempAdmin = false;
}

unsigned int CPlayerS2::GetSteamAccountID(bool validated)
{
	if (!IsAuthorized() || m_bFakeClient)
		return 0;
	return (unsigned int)(m_Xuid & 0xFFFFFFFF);
}

uint64_t CPlayerS2::GetSteamId64(bool validated)
{
	if (!IsAuthorized() || m_bFakeClient)
		return 0;
	return m_Xuid;
}

const char *CPlayerS2::GetSteam2Id(bool validated)
{
	if (!IsAuthorized())
		return nullptr;
	return m_Steam2Id.c_str();
}

const char *CPlayerS2::GetSteam3Id(bool validated)
{
	if (!IsAuthorized())
		return nullptr;
	return m_Steam3Id.c_str();
}

unsigned int CPlayerS2::GetOriginalLanguageId()
{
	return m_LangId;
}

/*********************
 * PlayerManagerS2   *
 *********************/

void PlayerManagerS2::OnSourceModAllInitialized()
{
	m_hookCount = 0;
	m_hookIds[m_hookCount++] = SH_ADD_HOOK(IServerGameClients, ClientConnect, serverClients,
		SH_MEMBER(this, &PlayerManagerS2::Hook_ClientConnect), false);
	m_hookIds[m_hookCount++] = SH_ADD_HOOK(IServerGameClients, OnClientConnected, serverClients,
		SH_MEMBER(this, &PlayerManagerS2::Hook_OnClientConnected), true);
	m_hookIds[m_hookCount++] = SH_ADD_HOOK(IServerGameClients, ClientPutInServer, serverClients,
		SH_MEMBER(this, &PlayerManagerS2::Hook_ClientPutInServer), true);
	m_hookIds[m_hookCount++] = SH_ADD_HOOK(IServerGameClients, ClientDisconnect, serverClients,
		SH_MEMBER(this, &PlayerManagerS2::Hook_ClientDisconnect), false);
	m_hookIds[m_hookCount++] = SH_ADD_HOOK(IServerGameClients, ClientDisconnect, serverClients,
		SH_MEMBER(this, &PlayerManagerS2::Hook_ClientDisconnect_Post), true);
	m_hookIds[m_hookCount++] = SH_ADD_HOOK(IServerGameClients, ClientSettingsChanged, serverClients,
		SH_MEMBER(this, &PlayerManagerS2::Hook_ClientSettingsChanged), true);

	sharesys->AddInterface(NULL, this);

	ParamType p1[] = {Param_Cell, Param_String, Param_Cell};
	ParamType p2[] = {Param_Cell};

	m_clconnect = forwardsys->CreateForward("OnClientConnect", ET_LowEvent, 3, p1);
	m_clconnect_post = forwardsys->CreateForward("OnClientConnected", ET_Ignore, 1, p2);
	m_clputinserver = forwardsys->CreateForward("OnClientPutInServer", ET_Ignore, 1, p2);
	m_cldisconnect = forwardsys->CreateForward("OnClientDisconnect", ET_Ignore, 1, p2);
	m_cldisconnect_post = forwardsys->CreateForward("OnClientDisconnect_Post", ET_Ignore, 1, p2);
	m_clinfochanged = forwardsys->CreateForward("OnClientSettingsChanged", ET_Ignore, 1, p2);
	m_clauth = forwardsys->CreateForward("OnClientAuthorized", ET_Ignore, 2, NULL, Param_Cell, Param_String);
	m_PreAdminCheck = forwardsys->CreateForward("OnClientPreAdminCheck", ET_Event, 1, p2);
	m_PostAdminCheck = forwardsys->CreateForward("OnClientPostAdminCheck", ET_Ignore, 1, p2);
	m_PostAdminFilter = forwardsys->CreateForward("OnClientPostAdminFilter", ET_Ignore, 1, p2);
}

void PlayerManagerS2::OnSourceModShutdown()
{
	for (int i = 0; i < m_hookCount; i++)
		SH_REMOVE_HOOK_ID(m_hookIds[i]);
	m_hookCount = 0;

	forwardsys->ReleaseForward(m_clconnect);
	forwardsys->ReleaseForward(m_clconnect_post);
	forwardsys->ReleaseForward(m_clputinserver);
	forwardsys->ReleaseForward(m_cldisconnect);
	forwardsys->ReleaseForward(m_cldisconnect_post);
	forwardsys->ReleaseForward(m_clinfochanged);
	forwardsys->ReleaseForward(m_clauth);
	forwardsys->ReleaseForward(m_PreAdminCheck);
	forwardsys->ReleaseForward(m_PostAdminCheck);
	forwardsys->ReleaseForward(m_PostAdminFilter);
}

void PlayerManagerS2::OnSourceModLevelChange(const char *mapName)
{
	m_ServerActivated = true;

	std::list<IClientListener *>::iterator iter;
	for (iter = m_hooks.begin(); iter != m_hooks.end(); iter++)
		(*iter)->OnServerActivated(GetMaxClients());
}

CPlayerS2 *PlayerManagerS2::GetPlayerBySlot(CPlayerSlot slot)
{
	int index = IndexOfSlot(slot);
	if (index < 1 || index > S2_MAX_PLAYERS)
		return nullptr;
	return &s_Players[index];
}

bool PlayerManagerS2::Hook_ClientConnect(CPlayerSlot slot, const char *pszName, uint64 xuid,
                                         const char *pszNetworkID, bool unk1,
                                         CBufferString *pRejectReason)
{
	int client = IndexOfSlot(slot);
	CPlayerS2 *pPlayer = GetPlayerBySlot(slot);
	if (pPlayer == nullptr)
		RETURN_META_VALUE(MRES_IGNORED, true);

	/* The engine does not provide the address in this callback; it arrives
	 * with OnClientConnected. Initialize now so listeners see a connected
	 * player, refresh later. */
	pPlayer->Initialize(client, pszName, xuid, pszNetworkID, "", false);

	char reject[255];
	reject[0] = '\0';

	std::list<IClientListener *>::iterator iter;
	for (iter = m_hooks.begin(); iter != m_hooks.end(); iter++)
	{
		if (!(*iter)->InterceptClientConnect(client, reject, sizeof(reject)))
		{
			if (reject[0] != '\0')
				pRejectReason->Insert(0, reject);
			pPlayer->Disconnect();
			RETURN_META_VALUE(MRES_SUPERCEDE, false);
		}
	}

	cell_t res = 1;
	if (m_clconnect != nullptr && m_clconnect->GetFunctionCount() > 0)
	{
		m_clconnect->PushCell(client);
		m_clconnect->PushStringEx(reject, sizeof(reject), SM_PARAM_STRING_UTF8, SM_PARAM_COPYBACK);
		m_clconnect->PushCell(sizeof(reject));
		m_clconnect->Execute(&res);
	}

	if (res == 0)
	{
		if (reject[0] != '\0')
			pRejectReason->Insert(0, reject);
		pPlayer->Disconnect();
		RETURN_META_VALUE(MRES_SUPERCEDE, false);
	}

	RETURN_META_VALUE(MRES_IGNORED, true);
}

void PlayerManagerS2::Hook_OnClientConnected(CPlayerSlot slot, const char *pszName, uint64 xuid,
                                             const char *pszNetworkID, const char *pszAddress,
                                             bool bFakePlayer)
{
	int client = IndexOfSlot(slot);
	CPlayerS2 *pPlayer = GetPlayerBySlot(slot);
	if (pPlayer == nullptr)
		return;

	/* Bots never went through ClientConnect; humans refresh their data
	 * (this callback carries the address). */
	pPlayer->Initialize(client, pszName, xuid, pszNetworkID, pszAddress, bFakePlayer);

	std::list<IClientListener *>::iterator iter;
	for (iter = m_hooks.begin(); iter != m_hooks.end(); iter++)
		(*iter)->OnClientConnected(client);

	if (m_clconnect_post != nullptr && m_clconnect_post->GetFunctionCount() > 0)
	{
		m_clconnect_post->PushCell(client);
		m_clconnect_post->Execute(NULL);
	}
}

void PlayerManagerS2::AuthorizePlayer(CPlayerS2 *pPlayer)
{
	if (pPlayer->m_bAuthorized)
		return;

	pPlayer->m_bAuthorized = true;
	int client = pPlayer->GetIndex();
	const char *auth = pPlayer->m_Steam2Id.c_str();

	std::list<IClientListener *>::iterator iter;
	for (iter = m_hooks.begin(); iter != m_hooks.end(); iter++)
		(*iter)->OnClientAuthorized(client, auth);

	if (m_clauth != nullptr && m_clauth->GetFunctionCount() > 0)
	{
		m_clauth->PushCell(client);
		m_clauth->PushString(auth);
		m_clauth->Execute(NULL);
	}

	/* Admin pipeline: pre-check forward, cache lookup, post-check. */
	cell_t res = 1; /* Plugin_Continue */
	if (m_PreAdminCheck != nullptr && m_PreAdminCheck->GetFunctionCount() > 0)
	{
		m_PreAdminCheck->PushCell(client);
		m_PreAdminCheck->Execute(&res);
	}
	if (res == 1)
	{
		pPlayer->RunAdminCacheChecks();
		pPlayer->NotifyPostAdminChecks();
	}
}

void PlayerManagerS2::NotifyPostAdminChecks(CPlayerS2 *pPlayer)
{
	int client = pPlayer->GetIndex();

	if (m_PostAdminFilter != nullptr && m_PostAdminFilter->GetFunctionCount() > 0)
	{
		m_PostAdminFilter->PushCell(client);
		m_PostAdminFilter->Execute(NULL);
	}
	if (m_PostAdminCheck != nullptr && m_PostAdminCheck->GetFunctionCount() > 0)
	{
		m_PostAdminCheck->PushCell(client);
		m_PostAdminCheck->Execute(NULL);
	}
}

void PlayerManagerS2::Hook_ClientPutInServer(CPlayerSlot slot, char const *pszName, int type,
                                             uint64 xuid)
{
	int client = IndexOfSlot(slot);
	CPlayerS2 *pPlayer = GetPlayerBySlot(slot);
	if (pPlayer == nullptr)
		return;

	if (!pPlayer->IsConnected())
	{
		/* Defensive: a client we never saw connect (e.g. core loaded late). */
		pPlayer->Initialize(client, pszName, xuid, "", "", type == 1);
	}

	pPlayer->m_bInGame = true;

	/* The XUID is known by now; this is where authorization is fired.
	 * Source2 WIP: no Steam ticket validation round-trip yet. */
	AuthorizePlayer(pPlayer);

	std::list<IClientListener *>::iterator iter;
	for (iter = m_hooks.begin(); iter != m_hooks.end(); iter++)
		(*iter)->OnClientPutInServer(client);

	if (m_clputinserver != nullptr && m_clputinserver->GetFunctionCount() > 0)
	{
		m_clputinserver->PushCell(client);
		m_clputinserver->Execute(NULL);
	}
}

void PlayerManagerS2::Hook_ClientDisconnect(CPlayerSlot slot, ENetworkDisconnectionReason reason,
                                            const char *pszName, uint64 xuid,
                                            const char *pszNetworkID)
{
	int client = IndexOfSlot(slot);
	CPlayerS2 *pPlayer = GetPlayerBySlot(slot);
	if (pPlayer == nullptr || !pPlayer->IsConnected())
		RETURN_META(MRES_IGNORED);

	std::list<IClientListener *>::iterator iter;
	for (iter = m_hooks.begin(); iter != m_hooks.end(); iter++)
		(*iter)->OnClientDisconnecting(client);

	if (m_cldisconnect != nullptr && m_cldisconnect->GetFunctionCount() > 0)
	{
		m_cldisconnect->PushCell(client);
		m_cldisconnect->Execute(NULL);
	}

	RETURN_META(MRES_IGNORED);
}

void PlayerManagerS2::Hook_ClientDisconnect_Post(CPlayerSlot slot,
                                                 ENetworkDisconnectionReason reason,
                                                 const char *pszName, uint64 xuid,
                                                 const char *pszNetworkID)
{
	int client = IndexOfSlot(slot);
	CPlayerS2 *pPlayer = GetPlayerBySlot(slot);
	if (pPlayer == nullptr || !pPlayer->IsConnected())
		return;

	pPlayer->Disconnect();

	std::list<IClientListener *>::iterator iter;
	for (iter = m_hooks.begin(); iter != m_hooks.end(); iter++)
		(*iter)->OnClientDisconnected(client);

	if (m_cldisconnect_post != nullptr && m_cldisconnect_post->GetFunctionCount() > 0)
	{
		m_cldisconnect_post->PushCell(client);
		m_cldisconnect_post->Execute(NULL);
	}
}

void PlayerManagerS2::Hook_ClientSettingsChanged(CPlayerSlot slot)
{
	int client = IndexOfSlot(slot);
	CPlayerS2 *pPlayer = GetPlayerBySlot(slot);
	if (pPlayer == nullptr || !pPlayer->IsConnected())
		return;

	const char *name = engine->GetClientConVarValue(slot, "name");
	if (name != nullptr && name[0] != '\0')
		pPlayer->SetName(name);

	if (m_clinfochanged != nullptr && m_clinfochanged->GetFunctionCount() > 0)
	{
		m_clinfochanged->PushCell(client);
		m_clinfochanged->Execute(NULL);
	}
}

/* IPlayerManager */

void PlayerManagerS2::AddClientListener(IClientListener *listener)
{
	m_hooks.push_back(listener);
}

void PlayerManagerS2::RemoveClientListener(IClientListener *listener)
{
	m_hooks.remove(listener);
}

IGamePlayer *PlayerManagerS2::GetGamePlayer(int client)
{
	if (client < 1 || client > S2_MAX_PLAYERS)
		return nullptr;
	return &s_Players[client];
}

IGamePlayer *PlayerManagerS2::GetGamePlayer(edict_t *pEdict)
{
	/* No edicts on Source2. */
	return nullptr;
}

int PlayerManagerS2::GetMaxClients()
{
	if (gpGlobals != nullptr && gpGlobals->maxClients > 0)
		return gpGlobals->maxClients;
	return S2_MAX_PLAYERS;
}

int PlayerManagerS2::GetNumPlayers()
{
	int count = 0;
	for (int i = 1; i <= S2_MAX_PLAYERS; i++)
	{
		if (s_Players[i].IsInGame())
			count++;
	}
	return count;
}

int PlayerManagerS2::GetClientOfUserId(int userid)
{
	for (int i = 1; i <= S2_MAX_PLAYERS; i++)
	{
		if (s_Players[i].IsConnected() && s_Players[i].GetUserId() == userid)
			return i;
	}
	return 0;
}

bool PlayerManagerS2::IsServerActivated()
{
	return m_ServerActivated;
}

unsigned int PlayerManagerS2::GetReplyTo()
{
	return m_ReplyTo;
}

unsigned int PlayerManagerS2::SetReplyTo(unsigned int reply)
{
	unsigned int old = m_ReplyTo;
	m_ReplyTo = reply;
	return old;
}

int PlayerManagerS2::FilterCommandTarget(IGamePlayer *pAdmin, IGamePlayer *pTarget, int flags)
{
	if (pTarget == nullptr || !pTarget->IsConnected())
		return COMMAND_TARGET_NONE;
	if ((flags & COMMAND_FILTER_CONNECTED) == 0 && !pTarget->IsInGame())
		return COMMAND_TARGET_NOT_IN_GAME;
	if ((flags & COMMAND_FILTER_NO_BOTS) && pTarget->IsFakeClient())
		return COMMAND_TARGET_NOT_HUMAN;
	if (pAdmin != nullptr && adminsys != nullptr &&
	    !adminsys->CanAdminTarget(pAdmin->GetAdminId(), pTarget->GetAdminId()))
	{
		return COMMAND_TARGET_IMMUNE;
	}
	/* COMMAND_FILTER_ALIVE/DEAD need game entity state: unsupported yet. */
	return COMMAND_TARGET_VALID;
}

void PlayerManagerS2::RegisterCommandTargetProcessor(ICommandTargetProcessor *pHandler)
{
	m_targetProcessors.push_back(pHandler);
}

void PlayerManagerS2::UnregisterCommandTargetProcessor(ICommandTargetProcessor *pHandler)
{
	m_targetProcessors.remove(pHandler);
}

int PlayerManagerS2::GetClientFromSerial(unsigned int serial)
{
	int index = (int)(serial & 0xFF);
	IGamePlayer *pPlayer = GetGamePlayer(index);
	if (pPlayer == nullptr || pPlayer->GetSerial() != serial)
		return 0;
	return index;
}

void PlayerManagerS2::ProcessCommandTarget(cmd_target_info_t *info)
{
	std::list<ICommandTargetProcessor *>::iterator iter;
	for (iter = m_targetProcessors.begin(); iter != m_targetProcessors.end(); iter++)
	{
		if ((*iter)->ProcessCommandTarget(info))
			return;
	}

	IGamePlayer *pAdmin = (info->admin > 0) ? GetGamePlayer(info->admin) : nullptr;

	info->num_targets = 0;
	info->reason = COMMAND_TARGET_NONE;

	/* Source2 WIP: single-target resolution only (#userid or exact name);
	 * multi-target patterns (@all, @bots, ...) are not implemented. */
	int found = 0;
	if (info->pattern[0] == '#')
	{
		int userid = atoi(&info->pattern[1]);
		if (userid > 0)
			found = GetClientOfUserId(userid);
	}
	if (found == 0)
	{
		for (int i = 1; i <= S2_MAX_PLAYERS; i++)
		{
			if (s_Players[i].IsConnected() &&
			    strcmp(s_Players[i].GetName(), info->pattern) == 0)
			{
				found = i;
				break;
			}
		}
	}

	if (found == 0)
		return;

	IGamePlayer *pTarget = GetGamePlayer(found);
	int reason = FilterCommandTarget(pAdmin, pTarget, info->flags);
	info->reason = reason;
	if (reason != COMMAND_TARGET_VALID)
		return;

	info->targets[0] = found;
	info->num_targets = 1;
	ke::SafeStrcpy(info->target_name, info->target_name_maxlength, pTarget->GetName());
	info->target_name_style = COMMAND_TARGETNAME_RAW;
}

void PlayerManagerS2::ClearAdminId(AdminId id)
{
	for (int i = 1; i <= S2_MAX_PLAYERS; i++)
	{
		if (s_Players[i].GetAdminId() == id)
			s_Players[i].ClearAdmin();
	}
}

void PlayerManagerS2::RecheckAnyAdmins()
{
	for (int i = 1; i <= S2_MAX_PLAYERS; i++)
	{
		CPlayerS2 *pPlayer = &s_Players[i];
		if (!pPlayer->IsInGame() || !pPlayer->IsAuthorized())
			continue;
		pPlayer->ClearAdmin();
		pPlayer->RunAdminCacheChecks();
	}
}

#endif // SOURCE2_WIP
