#include "riivolution.h"
#include "riivolution_config.h"

#include <files.h>

#include <unistd.h>

#define OPEN_MODE_BYPASS 0x80

static int fd = -1;
static DiscNode* fst = NULL;
static bool shiftfiles = false;
static u64 shift = 0;
static u32 fstsize;

using std::string;

namespace Ioctl { enum Enum {
	AddFile			= 0xC1,
	AddPatch		= 0xC2,
	AddShift		= 0xC3,
	SetClusters		= 0xC4,
	Allocate		= 0xC5
}; }

int RVL_Initialize()
{
	if (fd < 0)
		fd = IOS_Open("/dev/do", OPEN_MODE_BYPASS);
	shift = 0x200000000ULL; // 8 GB shift
	return fd;
}

void RVL_Close()
{
	if (fd < 0)
		return;
	IOS_Close(fd);
}

void RVL_SetFST(void* address, u32 size)
{
	fst = (DiscNode*)address;
	fstsize = size;
}

u32 RVL_GetFSTSize()
{
	return fstsize;
}

int RVL_SetClusters(bool clusters)
{
	return IOS_Ioctl(fd, Ioctl::SetClusters, &clusters, sizeof(clusters), NULL, 0);
}

void RVL_SetAlwaysShift(bool shift)
{
	shiftfiles = shift;
}

int RVL_Allocate(PatchType::Enum type, int num)
{
	u32 command[2];
	command[0] = type;
	command[1] = num;
	return IOS_Ioctl(fd, Ioctl::Allocate, command, 8, NULL, 0);
}

int RVL_AddFile(const char* filename)
{
	return IOS_Ioctl(fd, Ioctl::AddFile, (void*)filename, strlen(filename) + 1, NULL, 0);
}

int RVL_AddFile(const char* filename, u64 identifier)
{
	return IOS_Ioctl(fd, Ioctl::AddFile, (void*)filename, strlen(filename) + 1, &identifier, 8);
}

int RVL_AddShift(u64 original, u64 offset, u32 length)
{
	u32 command[8];
	command[0] = length;
	command[1] = original >> 32;
	command[2] = original;
	command[3] = offset >> 32;
	command[4] = offset;
	return IOS_Ioctl(fd, Ioctl::AddShift, command, 0x20, NULL, 0);
}

int RVL_AddPatch(int file, u64 offset, u32 fileoffset, u32 length)
{
	u32 command[8];
	command[0] = file;
	command[1] = fileoffset;
	command[2] = offset >> 32;
	command[3] = offset;
	command[4] = length;
	return IOS_Ioctl(fd, Ioctl::AddPatch, command, 0x20, NULL, 0);
}

#define ROUND_UP(p, round) \
	((p + round - 1) & ~(round - 1))

u64 RVL_GetShiftOffset(u32 length)
{
	u64 ret = shift;
	shift = ROUND_UP(shift + length, 0x40);
	return ret;
}

#define REFRESH_PATH() { \
	strcpy(curpath, "/"); \
	if (nodes.size() > 1) \
		for (DiscNode** iter = nodes.Data() + 1; iter != nodes.End(); iter++) { \
			strcat(curpath, nametable + (*iter)->GetNameOffset()); \
			strcat(curpath, "/"); \
		} \
	pos = strlen(curpath); \
}

#if 0 // Old filenode
DiscNode* RVL_FindNode(const char* fstname)
{
	const char* nametable = (const char*)(fst + fst->Size);
	
	u32 count = 1;
	DiscNode*> nodes;
	char curpath[MAXPATHLEN];
	int pos = 0;
	
	REFRESH_PATH();
	
	for (DiscNode* node = fst; (void*)node < (void*)nametable; node++, count++) {
		const char* name = nametable + node->GetNameOffset();
		
		if (node->Type == 0x01) {
			nodes.push_back(node);
			
			REFRESH_PATH();
		} else if (node->Type == 0x00) {
			curpath[pos] = '\0'; // Null-terminate it to the path
			strcat(curpath, name);
			
			if (!strcasecmp(curpath, fstname) || !strcasecmp(name, fstname))
				return node;
		}
		
		while (nodes.size() > 0 && count == nodes.back()->Size) {
			nodes.pop_back();
			
			REFRESH_PATH();
		}
	}
	
	return NULL;
}
#endif

static DiscNode* RVL_FindNode(const char* name, DiscNode* root, bool recursive)
{
	const char* nametable = (const char*)(fst + fst->Size);
	int offset = root - fst;
	DiscNode* node = root;
	while ((void*)node < (void*)nametable) {
		if (!strcmp(nametable + node->GetNameOffset(), name))
			return node;

		if (recursive || node->Type == 0)
			node++;
		else
			node = root + node->Size - offset;
	}

	return NULL;
}

DiscNode* RVL_FindNode(const char* fstname)
{
	if (fstname[0] != '/')
		return RVL_FindNode(fstname, fst, true);

	char namebuffer[MAXPATHLEN];
	char* name = namebuffer;
	strcpy(name, fstname + 1);

	DiscNode* root = fst;
	while (root) {
		char* slash = strchr(name, '/');
		if (!slash)
			return RVL_FindNode(name, root, false);

		*slash = '\0';
		root = RVL_FindNode(name, root, false);
		name = slash + 1;
	}

	return NULL;
}

static void ShiftFST(DiscNode* node)
{
	node->DataOffset = (u32)(RVL_GetShiftOffset(node->Size) >> 2);
}

static void ResizeFST(DiscNode* node, u32 length, bool telldip)
{
	u64 oldoffset = (u64)node->DataOffset << 2;
	u32 oldsize = node->Size;
	node->Size = length;

	if (length > oldsize || shiftfiles)
		ShiftFST(node);

	if (telldip)
		RVL_AddShift(oldoffset, (u64)node->DataOffset << 2, oldsize);
}

static void RVL_Patch(RiiFilePatch* file, bool stat, u64 externalid, string commonfs)
{
	DiscNode* node = RVL_FindNode(file->Disc.c_str());

	if (!node) {
		if (file->Create) {
			// TODO: Add FST entries
		} else
			return;
	}

	string external = file->External;
	if (commonfs.size() && !commonfs.compare(0, commonfs.size(), external))
		external = external.substr(commonfs.size());

	if (file->Length == 0) {
		Stats st;
		if (File_Stat(external.c_str(), &st) == 0) {
			file->Length = st.Size;
			stat = true;
			externalid = st.Identifier;
		} else
			return;
	}

	int fd;
	if (stat)
		fd = RVL_AddFile(external.c_str(), externalid);
	else
		fd = RVL_AddFile(external.c_str());

	if (fd >= 0) {
		bool shifted = false;

		if (file->Resize) {
			if (file->Length + file->Offset > node->Size) {
				ResizeFST(node, file->Length + file->Offset, file->Offset > 0);
				shifted = true;
			} else
				node->Size = file->Length + file->Offset;
		}

		if (!shifted && shiftfiles)
			ShiftFST(node);

		RVL_AddPatch(fd, ((u64)node->DataOffset << 2) + file->Offset, file->FileOffset, file->Length);
	} else
		exit(0);
}

static void RVL_Patch(RiiFilePatch* file, string commonfs)
{
	RVL_Patch(file, false, 0, commonfs);
}

static void RVL_Patch(RiiFolderPatch* folder, string commonfs)
{
	string external = folder->External;
	if (commonfs.size() && !commonfs.compare(0, commonfs.size(), external))
		external = external.substr(commonfs.size());

	char fdirname[MAXPATHLEN];
	int fdir = File_OpenDir(external.c_str());
	Stats stats;
	while (!File_NextDir(fdir, fdirname, &stats)) {
		if (fdirname[0] == '.')
			continue;
		if (stats.Mode & S_IFDIR) {
			if (folder->Recursive) {
				RiiFolderPatch newfolder = *folder;
				newfolder.Disc = PathCombine(newfolder.Disc, fdirname);
				newfolder.External = PathCombine(external, fdirname);
				RVL_Patch(&newfolder, commonfs);
			}
		} else {
			RiiFilePatch file;
			file.Create = folder->Create;
			file.Resize = folder->Resize;
			file.Offset = 0;
			file.FileOffset = 0;
			file.Length = stats.Size;
			file.Disc = PathCombine(folder->Disc, fdirname);
			file.External = PathCombine(external, fdirname);
			RVL_Patch(&file, true, stats.Identifier, commonfs);
		}
	}
	File_CloseDir(fdir);
}

static void RVL_Patch(RiiSavegamePatch* save)
{
	// TODO: This
}

static void ApplyParams(std::string* str, Map<string, string>* params)
{
	bool found = false;
	string::size_type pos;
	while ((pos = str->find("{$")) != string::npos) {
		string::size_type pend = str->find("}", pos);
		string param = str->substr(pos + 2, pend - pos - 2);
		*str = str->substr(0, pos) + (*params)[param] + str->substr(pend + 1);
		found = true;
	}
}

static void RVL_Patch(RiiPatch* patch, Map<string, string>* params, string commonfs)
{
	for (RiiFilePatch* file = patch->Files.Data(); file != patch->Files.End(); file++) {
		RiiFilePatch temp = *file;
		ApplyParams(&temp.External, params);
		ApplyParams(&temp.Disc, params);
		RVL_Patch(&temp, commonfs);
	}
	for (RiiFolderPatch* folder = patch->Folders.Data(); folder != patch->Folders.End(); folder++) {
		RiiFolderPatch temp = *folder;
		ApplyParams(&temp.External, params);
		ApplyParams(&temp.Disc, params);
		RVL_Patch(&temp, commonfs);
	}
	for (RiiSavegamePatch* save = patch->Savegames.Data(); save != patch->Savegames.End(); save++) {
		RiiSavegamePatch temp = *save;
		ApplyParams(&temp.External, params);
		RVL_Patch(&temp);
	}
}

void RVL_Patch(RiiDisc* disc)
{
	// Search for a common filesystem so we can optimize memory
	string filesystem;
	for (RiiSection* section = disc->Sections.Data(); section != disc->Sections.End(); section++) {
			for (RiiOption* option = section->Options.Data(); option != section->Options.End(); option++) {
				if (option->Default == 0)
					continue;
				RiiChoice* choice = &option->Choices[option->Default - 1];
				if (!filesystem.size())
					filesystem = choice->Filesystem;
				else if (!filesystem.compare(choice->Filesystem)) {
					filesystem = string();
					goto no_common_fs;
				}
			}
	}
no_common_fs:

	if (filesystem.size()) {
		File_SetDefaultPath(filesystem.c_str());
		RVL_SetClusters(true);
	}

	for (RiiSection* section = disc->Sections.Data(); section != disc->Sections.End(); section++) {
		for (RiiOption* option = section->Options.Data(); option != section->Options.End(); option++) {
			if (option->Default == 0)
				continue;
			Map<string, string> params;
			params.Add(option->Params.Data(), option->Params.Size());
			RiiChoice* choice = &option->Choices[option->Default - 1];
			params.Add(choice->Params.Data(), choice->Params.Size());
			int end = params.Size();
			for (RiiChoice::Patch* patch = choice->Patches.Data(); patch != choice->Patches.End(); patch++) {
				params.Add(patch->Params.Data(), patch->Params.Size());

				RVL_Patch(&disc->Patches[patch->ID], &params, filesystem);

				params.Delete(params.Data() + end, patch->Params.End());
			}
		}
	}
}

static void RVL_Patch(RiiMemoryPatch* memory)
{
	if (memory->Original && memcmp((void*)memory->Offset, memory->Original, memory->Length))
		return;

	memcpy((void*)memory->Offset, memory->Value, memory->Length);
}

void RVL_PatchMemory(RiiDisc* disc)
{
	for (RiiSection* section = disc->Sections.Data(); section != disc->Sections.End(); section++) {
		for (RiiOption* option = section->Options.Data(); option != section->Options.End(); option++) {
			if (option->Default == 0)
				continue;
			RiiChoice* choice = &option->Choices[option->Default - 1];
			for (RiiChoice::Patch* patch = choice->Patches.Data(); patch != choice->Patches.End(); patch++) {
				RiiPatch* mem = &disc->Patches[patch->ID];
				for (RiiMemoryPatch* memory = mem->Memory.Data(); memory != mem->Memory.End(); memory++) {
					RVL_Patch(memory);
				}
			}
		}
	}
}