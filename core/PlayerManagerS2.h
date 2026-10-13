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

#ifndef _INCLUDE_SOURCEMOD_PLAYERMANAGER_S2_H_
#define _INCLUDE_SOURCEMOD_PLAYERMANAGER_S2_H_

#ifdef SOURCE2_WIP

/**
 * Source2 (CS2) player manager.
 *
 * A fresh implementation of SourceMod::IPlayerManager on top of
 * ISource2GameClients (CPlayerSlot based) instead of porting the edict_t
 * based PlayerManager. SourceMod client indices are slot+1 (1-based).
 *
 * Scoped-down semantics, reported rather than faked:
 *  - GetEdict() / GetPlayerInfo() return nullptr (no edicts on Source2).
 *  - Steam auth strings are derived from the XUID the engine reports at
 *    connect time; Steam ticket validation is NOT wired up yet, so the
 *    "validated" flag of auth getters is satisfied as soon as the engine
 *    gave us a XUID.
 *  - ProcessCommandTarget only resolves single targets (#userid or exact
 *    name); multi-target patterns (@all, @me, ...) are not implemented.
 *  - No OnClientLanguageChanged (language stays the server default unless
 *    SetLanguageId is called), no client command KeyValues forwards.
 */

#include "sm_globals.h"
#include <IPlayerHelpers.h>
#include <IAdminSystem.h>
#include <IForwardSys.h>
#include <eiface.h>

#include <list>
#include <string>

class PlayerManagerS2;

class CPlayerS2 : public IGamePlayer
{
	friend class PlayerManagerS2;

public:
	CPlayerS2();

public: // IGamePlayer
	const char *GetName() override;
	const char *GetIPAddress() override;
	const char *GetAuthString(bool validated = true) override;
	edict_t *GetEdict() override;
	bool IsInGame() override;
	bool IsConnected() override;
	bool IsFakeClient() override;
	bool IsSourceTV() const override;
	bool IsReplay() const override;
	AdminId GetAdminId() override;
	void SetAdminId(AdminId id, bool temp) override;
	int GetUserId() override;
	unsigned int GetLanguageId() override;
	void SetLanguageId(unsigned int id) override;
	IPlayerInfo *GetPlayerInfo() override;
	bool RunAdminCacheChecks() override;
	void NotifyPostAdminChecks() override;
	unsigned int GetSerial() override;
	int GetIndex() const override;
	bool IsAuthorized() override;
	void Kick(const char *message) override;
	bool IsInKickQueue() override;
	void MarkAsBeingKicked() override;
	void PrintToConsole(const char *pMsg) override;
	void ClearAdmin() override;
	unsigned int GetSteamAccountID(bool validated = true) override;
	uint64_t GetSteamId64(bool validated = true) override;
	const char *GetSteam2Id(bool validated = true) override;
	const char *GetSteam3Id(bool validated = true) override;
	unsigned int GetOriginalLanguageId() override;

private:
	void Initialize(int index, const char *name, uint64_t xuid,
	                const char *networkid, const char *address, bool fake);
	void Disconnect();
	void SetName(const char *name);
	void DoBasicAdminChecks();
	CPlayerSlot Slot() const { return CPlayerSlot(m_iIndex - 1); }

private:
	int m_iIndex = 0;
	bool m_bConnected = false;
	bool m_bInGame = false;
	bool m_bFakeClient = false;
	bool m_bAuthorized = false;
	bool m_bAdminCheckSignalled = false;
	bool m_bInKickQueue = false;
	std::string m_Name;
	std::string m_Ip;         /* without port */
	std::string m_IpAndPort;
	std::string m_NetworkId;
	std::string m_Steam2Id;
	std::string m_Steam3Id;
	uint64_t m_Xuid = 0;
	int m_UserId = -1;
	unsigned int m_Serial = 0;
	unsigned int m_LangId = 0;
	AdminId m_Admin = INVALID_ADMIN_ID;
	bool m_TempAdmin = false;
};

class PlayerManagerS2 :
	public SMGlobalClass,
	public IPlayerManager
{
public:
	/* SMGlobalClass */
	void OnSourceModAllInitialized() override;
	void OnSourceModShutdown() override;
	void OnSourceModLevelChange(const char *mapName) override;

public: // IPlayerManager
	void AddClientListener(IClientListener *listener) override;
	void RemoveClientListener(IClientListener *listener) override;
	IGamePlayer *GetGamePlayer(int client) override;
	IGamePlayer *GetGamePlayer(edict_t *pEdict) override;
	int GetMaxClients() override;
	int GetNumPlayers() override;
	int GetClientOfUserId(int userid) override;
	bool IsServerActivated() override;
	unsigned int GetReplyTo() override;
	unsigned int SetReplyTo(unsigned int reply) override;
	int FilterCommandTarget(IGamePlayer *pAdmin, IGamePlayer *pTarget, int flags) override;
	void RegisterCommandTargetProcessor(ICommandTargetProcessor *pHandler) override;
	void UnregisterCommandTargetProcessor(ICommandTargetProcessor *pHandler) override;
	int GetClientFromSerial(unsigned int serial) override;
	void ProcessCommandTarget(cmd_target_info_t *info) override;
	void ClearAdminId(AdminId id) override;
	void RecheckAnyAdmins() override;

public: /* ISource2GameClients hook handlers */
	bool Hook_ClientConnect(CPlayerSlot slot, const char *pszName, uint64 xuid,
	                        const char *pszNetworkID, bool unk1, CBufferString *pRejectReason);
	void Hook_OnClientConnected(CPlayerSlot slot, const char *pszName, uint64 xuid,
	                            const char *pszNetworkID, const char *pszAddress, bool bFakePlayer);
	void Hook_ClientPutInServer(CPlayerSlot slot, char const *pszName, int type, uint64 xuid);
	void Hook_ClientDisconnect(CPlayerSlot slot, ENetworkDisconnectionReason reason,
	                           const char *pszName, uint64 xuid, const char *pszNetworkID);
	void Hook_ClientDisconnect_Post(CPlayerSlot slot, ENetworkDisconnectionReason reason,
	                                const char *pszName, uint64 xuid, const char *pszNetworkID);
	void Hook_ClientSettingsChanged(CPlayerSlot slot);

public:
	CPlayerS2 *GetPlayerBySlot(CPlayerSlot slot);
	void NotifyPostAdminChecks(CPlayerS2 *pPlayer);

private:
	void AuthorizePlayer(CPlayerS2 *pPlayer);
	int IndexOfSlot(CPlayerSlot slot) const { return slot.Get() + 1; }

private:
	std::list<IClientListener *> m_hooks;
	std::list<ICommandTargetProcessor *> m_targetProcessors;
	IForward *m_clconnect = nullptr;
	IForward *m_clconnect_post = nullptr;
	IForward *m_clputinserver = nullptr;
	IForward *m_cldisconnect = nullptr;
	IForward *m_cldisconnect_post = nullptr;
	IForward *m_clinfochanged = nullptr;
	IForward *m_clauth = nullptr;
	IForward *m_PreAdminCheck = nullptr;
	IForward *m_PostAdminCheck = nullptr;
	IForward *m_PostAdminFilter = nullptr;
	unsigned int m_ReplyTo = 0; /* SM_REPLY_CONSOLE */
	bool m_ServerActivated = false;
	int m_hookIds[12];
	int m_hookCount = 0;

	friend class CPlayerS2;
};

extern PlayerManagerS2 g_PlayersS2;

#endif // SOURCE2_WIP
#endif // _INCLUDE_SOURCEMOD_PLAYERMANAGER_S2_H_
