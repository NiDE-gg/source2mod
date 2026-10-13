/**
 * SourceMod (Source2 / CS2 port)
 * Copyright (C) 2004-2026 AlliedModders LLC. All rights reserved.
 * Licensed under the GNU GPL, version 3, with the SourceMod
 * linking exception. See LICENSE.txt for details.
 */

#ifdef SOURCE2_WIP
#include <interfaces/interfaces.h>
#include <entity2/entitysystem.h>
#include <entity2/entityinstance.h>
#include "sourcemod.h"
#include "sourcemm_api.h"
#include "sm_autonatives.h"
#include "logic_bridge.h"
#include "PlayerManagerS2.h"
#include <IGameConfigs.h>
#include <IPlayerHelpers.h>
#include <sys/uio.h>
#include <unistd.h>

/* Validate the gamedata-derived pointer before dereferencing an object whose
 * layout is not published by the SDK. Linux Itanium RTTI was checked live.
 * A stale gamedata offset must fail explicitly rather than crash the server. */
static bool ReadPointer(const void *address, void **out)
{
	iovec local = {out, sizeof(*out)};
	iovec remote = {const_cast<void *>(address), sizeof(*out)};
	return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == sizeof(*out);
}

static bool IsEntitySystem(void *object)
{
	void *vtable, *typeinfo, *name;
	if (!ReadPointer(object, &vtable) || !vtable ||
	    !ReadPointer(static_cast<char *>(vtable) - sizeof(void *), &typeinfo) || !typeinfo ||
	    !ReadPointer(static_cast<char *>(typeinfo) + sizeof(void *), &name))
		return false;
	char actual[sizeof("17CGameEntitySystem")];
	iovec local = {actual, sizeof(actual)};
	iovec remote = {name, sizeof(actual)};
	return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == sizeof(actual) &&
	       memcmp(actual, "17CGameEntitySystem", sizeof(actual)) == 0;
}

static CEntitySystem *EntitySystem(IPluginContext *context)
{
	int offset;
	if (!g_pGameConf || !g_pGameConf->GetOffset("S2GameEntitySystem", &offset) ||
	    offset < 0 || offset > 4096 || offset % sizeof(void *) != 0)
	{
		context->ThrowNativeError("Source2 entity system gamedata is missing or invalid");
		return nullptr;
	}
	void *service = g_SMAPI->GetEngineFactory()(GAMERESOURCESERVICESERVER_INTERFACE_VERSION, nullptr);
	void *system = nullptr;
	if (!service || !ReadPointer(static_cast<char *>(service) + offset, &system) ||
	    !system || !IsEntitySystem(system))
	{
		context->ThrowNativeError("Source2 entity system is unavailable or gamedata does not match");
		return nullptr;
	}
	return static_cast<CEntitySystem *>(system);
}

/* Same identity lookup as SDK entity2/entitysystem.cpp, using its published
 * layouts. Handles are checked separately to reject recycled pawn indices. */
static CEntityInstance *EntityAt(CEntitySystem *system, int index)
{
	if (index < 0 || index >= MAX_TOTAL_ENTITIES - 1)
		return nullptr;
	auto *chunk = system->m_EntityList.m_pIdentityChunks[index / MAX_ENTITIES_IN_LIST];
	if (!chunk)
		return nullptr;
	auto *identity = &chunk[index % MAX_ENTITIES_IN_LIST];
	if (identity->GetEntityIndex().Get() != index ||
	    (identity->m_flags & (EF_IS_INVALID_EHANDLE | EF_DELETE_IN_PROGRESS | EF_MARKED_FOR_DELETE)))
		return nullptr;
	return identity->m_pInstance;
}

/* Resolve a field in its declaring class, including each base subobject's
 * offset. Do not infer layout from a similarly named field in another class. */
static bool FindField(CSchemaClassInfo *type, const char *owner, const char *name,
                      int width, int *offset, int depth = 0)
{
	if (!type || depth >= 32 || type->m_nSize < width)
		return false;
	if (strcmp(type->m_pszName, owner) == 0)
	{
		for (int i = 0; i < type->m_nFieldCount; i++)
		{
			auto &field = type->m_pFields[i];
			if (strcmp(field.m_pszName, name) != 0)
				continue;
			int size = 0;
			uint8 alignment = 0;
			if (!field.m_pType || !field.m_pType->GetSizeAndAlignment(size, alignment) || size != width ||
			    field.m_nSingleInheritanceOffset < 0 || field.m_nSingleInheritanceOffset > type->m_nSize - width)
				return false;
			*offset = field.m_nSingleInheritanceOffset;
			return true;
		}
		return false;
	}
	for (int i = 0; i < type->m_nBaseClassCount; i++)
	{
		auto &base = type->m_pBaseClasses[i];
		int relative;
		if (FindField(base.m_pClass, owner, name, width, &relative, depth + 1) &&
		    base.m_nOffset <= static_cast<unsigned int>(type->m_nSize - width) &&
		    relative <= type->m_nSize - width - static_cast<int>(base.m_nOffset))
		{
			*offset = static_cast<int>(base.m_nOffset) + relative;
			return true;
		}
	}
	return false;
}

template <typename T>
static bool ReadField(IPluginContext *context, CEntityInstance *entity,
                       const char *owner, const char *name, T *out)
{
	int offset;
	if (!FindField(entity->Schema_DynamicBinding().Get(), owner, name, sizeof(T), &offset))
	{
		context->ThrowNativeError("Source2 schema field %s::%s is missing or has an incompatible layout", owner, name);
		return false;
	}
	memcpy(out, reinterpret_cast<char *>(entity) + offset, sizeof(T));
	return true;
}

static CEntityInstance *Controller(IPluginContext *context, int client, CEntitySystem **system)
{
	auto *player = g_PlayersS2.GetGamePlayer(client);
	if (client <= 0 || !player)
	{
		context->ThrowNativeError("Client index %d is invalid", client);
		return nullptr;
	}
	if (!player->IsInGame())
	{
		context->ThrowNativeError("Client %d is not in game", client);
		return nullptr;
	}
	*system = EntitySystem(context);
	if (!*system)
		return nullptr;
	auto *entity = EntityAt(*system, client);
	if (!entity)
		context->ThrowNativeError("Client %d has no controller entity", client);
	return entity;
}

static bool PlayerPawn(IPluginContext *context, CEntitySystem *system,
                       CEntityInstance *controller, CEntityInstance **pawn)
{
	CEntityHandle handle;
	if (!ReadField(context, controller, "CCSPlayerController", "m_hPlayerPawn", &handle))
		return false;
	*pawn = handle.IsValid() ? EntityAt(system, handle.GetEntryIndex()) : nullptr;
	if (*pawn && (*pawn)->GetRefEHandle() != handle)
		*pawn = nullptr;
	return true;
}

static cell_t GetClientTeam(IPluginContext *context, const cell_t *params)
{
	CEntitySystem *system;
	auto *controller = Controller(context, params[1], &system);
	uint8 team;
	return controller && ReadField(context, controller, "CBaseEntity", "m_iTeamNum", &team) ? team : 0;
}

static cell_t GetClientHealth(IPluginContext *context, const cell_t *params)
{
	CEntitySystem *system;
	auto *controller = Controller(context, params[1], &system);
	CEntityInstance *pawn;
	if (!controller || !PlayerPawn(context, system, controller, &pawn))
		return 0;
	if (!pawn)
		return context->ThrowNativeError("Client %d has no player pawn", params[1]);
	int32 health;
	return ReadField(context, pawn, "CBaseEntity", "m_iHealth", &health) ? health : 0;
}

static cell_t IsPlayerAlive(IPluginContext *context, const cell_t *params)
{
	CEntitySystem *system;
	auto *controller = Controller(context, params[1], &system);
	CEntityInstance *pawn;
	if (!controller || !PlayerPawn(context, system, controller, &pawn) || !pawn)
		return 0;
	uint8 life;
	return ReadField(context, pawn, "CBaseEntity", "m_lifeState", &life) && life == LIFE_ALIVE;
}

REGISTER_NATIVES(playerStateS2Natives)
{
	{"GetClientTeam", GetClientTeam},
	{"GetClientHealth", GetClientHealth},
	{"IsPlayerAlive", IsPlayerAlive},
	{nullptr, nullptr}
};
#endif
