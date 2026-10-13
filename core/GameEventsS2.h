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

#ifndef _INCLUDE_SOURCEMOD_GAMEEVENTS_S2_H_
#define _INCLUDE_SOURCEMOD_GAMEEVENTS_S2_H_

#ifdef SOURCE2_WIP

#include <stddef.h>

/**
 * Source2 does not expose IGameEventManager2 through an interface factory.
 * Acquisition: locate the CGameEventManager vtable in the server module
 * through its RTTI (vtable lookup approach as used by CS2Fixes, GPLv3),
 * then find the static singleton: the unique object in .bss/.data whose
 * vptr is that primary vtable. Nothing is hardcoded; everything is derived
 * from the loaded binary at runtime, and verified live under gdb.
 *
 * Returns false (with a logged error) when resolution fails; event natives
 * then stay unavailable instead of faking support.
 */
bool S2GameEvents_Init(char *error, size_t maxlen);
void S2GameEvents_Shutdown();

#endif // SOURCE2_WIP
#endif // _INCLUDE_SOURCEMOD_GAMEEVENTS_S2_H_
