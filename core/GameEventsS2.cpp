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

#include "GameEventsS2.h"
#include "sourcemod.h"
#include "sourcemm_api.h"
#include "EventManager.h"
#include "logic_bridge.h"
#include <bridge/include/ILogger.h>

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <am-string.h>

struct ModuleRange
{
	uintptr_t start;
	uintptr_t size;
};

static ModuleRange s_Data = {}, s_Bss = {};

/* Reads the module's ELF section headers from disk to bound the in-memory
 * scans (sh_addr is the load-relative virtual address). */
static bool GetSectionRanges(const char *path, uintptr_t base,
                             ModuleRange *rodata, ModuleRange *datarelro)
/* also fills s_Data / s_Bss */
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return false;

	struct stat st;
	if (fstat(fd, &st) != 0)
	{
		close(fd);
		return false;
	}

	void *map = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED)
		return false;

	bool found_ro = false, found_relro = false;
	const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)map;
	if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) == 0 && ehdr->e_shoff != 0)
	{
		const Elf64_Shdr *shdrs = (const Elf64_Shdr *)((uintptr_t)map + ehdr->e_shoff);
		const char *shstr = (const char *)((uintptr_t)map + shdrs[ehdr->e_shstrndx].sh_offset);
		for (int i = 0; i < ehdr->e_shnum; i++)
		{
			const char *name = shstr + shdrs[i].sh_name;
			if (strcmp(name, ".rodata") == 0)
			{
				rodata->start = base + shdrs[i].sh_addr;
				rodata->size = shdrs[i].sh_size;
				found_ro = true;
			}
			else if (strcmp(name, ".data.rel.ro") == 0)
			{
				datarelro->start = base + shdrs[i].sh_addr;
				datarelro->size = shdrs[i].sh_size;
				found_relro = true;
			}
			else if (strcmp(name, ".data") == 0)
			{
				s_Data.start = base + shdrs[i].sh_addr;
				s_Data.size = shdrs[i].sh_size;
			}
			else if (strcmp(name, ".bss") == 0)
			{
				s_Bss.start = base + shdrs[i].sh_addr;
				s_Bss.size = shdrs[i].sh_size;
			}
		}
	}

	munmap(map, st.st_size);
	return found_ro && found_relro;
}

static const void *FindBytes(const ModuleRange &range, const void *needle, size_t len)
{
	return memmem((const void *)range.start, range.size, needle, len);
}

/* Itanium ABI: the vtable region is [offset-to-top][typeinfo*][fn0][fn1]...
 * and typeinfo's second qword points at the mangled class name. */
static void **FindVtableByClassName(const char *module_example_sym_owner,
                                    const char *mangled_name, char *error, size_t maxlen)
{
	/* Locate the server module from a pointer we already trust: the vtable
	 * of the ISource2Server instance Metamod handed us lives in it. */
	Dl_info info;
	void *server_vtable = *(void **)gamedll;
	if (dladdr(server_vtable, &info) == 0 || info.dli_fname == nullptr)
	{
		ke::SafeSprintf(error, maxlen, "could not resolve the server module");
		return nullptr;
	}
	uintptr_t base = (uintptr_t)info.dli_fbase;

	ModuleRange rodata = {}, relro = {};
	if (!GetSectionRanges(info.dli_fname, base, &rodata, &relro))
	{
		ke::SafeSprintf(error, maxlen, "could not read ELF sections of %s", info.dli_fname);
		return nullptr;
	}

	const void *name_addr = FindBytes(rodata, mangled_name, strlen(mangled_name) + 1);
	if (name_addr == nullptr)
	{
		ke::SafeSprintf(error, maxlen, "RTTI name %s not found", mangled_name);
		return nullptr;
	}

	/* typeinfo object: [vptr][name*]... -> find a pointer to the name. */
	const void *name_ref = FindBytes(relro, &name_addr, sizeof(void *));
	if (name_ref == nullptr)
	{
		ke::SafeSprintf(error, maxlen, "typeinfo for %s not found", mangled_name);
		return nullptr;
	}
	const void *typeinfo = (const void *)((uintptr_t)name_ref - sizeof(void *));

	/* vtable: [offset-to-top == 0][typeinfo*][functions...] -> find a
	 * pointer to the typeinfo preceded by a zero qword (the primary
	 * vtable; secondary bases have non-zero offsets). */
	ModuleRange search = relro;
	while (search.size >= sizeof(void *))
	{
		const void *ti_ref = FindBytes(search, &typeinfo, sizeof(void *));
		if (ti_ref == nullptr)
			break;

		uintptr_t at = (uintptr_t)ti_ref;
		if (*(const uintptr_t *)(at - sizeof(void *)) == 0)
			return (void **)(at + sizeof(void *));

		search.size = search.start + search.size - (at + sizeof(void *));
		search.start = at + sizeof(void *);
	}

	ke::SafeSprintf(error, maxlen, "primary vtable for %s not found", mangled_name);
	return nullptr;
}

bool S2GameEvents_Init(char *error, size_t maxlen)
{
	void **vtable = FindVtableByClassName("server", "17CGameEventManager", error, maxlen);
	if (vtable == nullptr)
		return false;

	/* The manager is a static singleton in the server module: the single
	 * object whose vptr is the primary CGameEventManager vtable (verified
	 * live under gdb: exactly one hit, in .bss). Its constructor ran when
	 * libserver initialized, long before SourceMod loads. */
	const void *vptr = (const void *)vtable;
	const void *obj = nullptr;
	if (s_Bss.size != 0)
		obj = FindBytes(s_Bss, &vptr, sizeof(void *));
	if (obj == nullptr && s_Data.size != 0)
		obj = FindBytes(s_Data, &vptr, sizeof(void *));

	if (obj == nullptr)
	{
		ke::SafeSprintf(error, maxlen, "CGameEventManager instance not found");
		return false;
	}

	gameevents = (IGameEventManager2 *)obj;
	logger->LogMessage("[SM] Game event manager resolved at %p", obj);
	return true;
}

void S2GameEvents_Shutdown()
{
}

#endif // SOURCE2_WIP
