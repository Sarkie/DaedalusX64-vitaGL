#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>

#include <vitasdk.h>
#include <vitaGL.h>

#include "BuildOptions.h"
#include "Config/ConfigOptions.h"
#include "Core/Cheats.h"
#include "Core/CPU.h"
#include "Core/Memory.h"
#include "Core/PIF.h"
#include "Core/RomSettings.h"
#include "Core/Save.h"
#include "Debug/DBGConsole.h"
#include "Debug/DebugLog.h"
#include "Graphics/GraphicsContext.h"
#include "HLEGraphics/TextureCache.h"
#include "Input/InputManager.h"
#include "Interface/RomDB.h"
#include "System/Paths.h"
#include "System/System.h"
#include "Test/BatchTest.h"
#include "Utility/IO.h"
#include "Utility/Preferences.h"
#include "Utility/Profiler.h"
#include "Utility/Thread.h"
#include "Utility/ROMFile.h"
#include "Utility/Timer.h"
#include "SysVita/UI/Menu.h"

#define STB_IMAGE_IMPLEMENTATION
#include "Utility/stb_image.h"

#define PREVIEW_PADDING 6
#define PREVIEW_HEIGHT 268.0f
#define PREVIEW_WIDTH  387.0f
#define MIN(x,y) ((x) > (y) ? (y) : (x))

#define ROMS_FOLDERS_NUM 5
#define FILTER_MODES_NUM 6

char selectedRom[256];
char rom_name_filter[128] = {0};

bool gHasOnlineRomList = false;

struct CompatibilityList {
	char name[128];
	bool playable;
	bool ingame_plus;
	bool ingame_low;
	bool crash;
	bool slow;
	CompatibilityList *next;
};

struct RomSelection {
	char name[128];
	char fullpath[256];
	bool is_online;
	RomSettings settings;
	RomID id;
	u32 size;
	ECicType cic;
	CompatibilityList *status;
	RomSelection *next;
};

static RomSelection *list = nullptr;
static CompatibilityList *comp = nullptr;
static RomSelection *old_hovered = nullptr;
static bool has_preview_icon = false;
static int preview_width, preview_height, preview_x, preview_y;
CRefPtr<CNativeTexture> mpPreviewTexture;
GLuint preview_icon = 0;

int oldSortOrder = -1;

int filter_idx = 0;
const char *filter_modes[] = {
	lang_strings[STR_NO_FILTER],
	lang_strings[STR_GAME_PLAYABLE],
	lang_strings[STR_GAME_INGAME_PLUS],
	lang_strings[STR_GAME_INGAME_MINUS],
	lang_strings[STR_GAME_CRASH],
	lang_strings[STR_NO_TAGS]
};

// Filter modes enum
enum {
	FILTER_DISABLED,
	FILTER_PLAYABLE,
	FILTER_INGAME_PLUS,
	FILTER_INGAME_MINUS,
	FILTER_CRASH,
	FILTER_NO_TAGS
};

void apply_rom_name_filter() {
	getDialogTextResult(rom_name_filter);
}

void resetRomList() {
	RomSelection *p = list;
	while (p) {
		RomSelection *old = p;
		p = p->next;
		free(old);
	}
	list = nullptr;
}

void swap_roms(RomSelection *a, RomSelection *b) { 
	char nametmp[128], pathtmp[256];
	sprintf(nametmp, a->name);
	sprintf(pathtmp, a->fullpath);

	RomSettings settingstmp = a->settings;
	RomID idtmp = a->id;
	u32 sizetmp = a->size;
	ECicType cictmp = a->cic;
	CompatibilityList *statustmp = a->status;
	bool onlinetmp = a->is_online;

	sprintf(a->name, b->name);
	sprintf(a->fullpath, b->fullpath);
	
	a->is_online = b->is_online;
	a->settings = b->settings;
	a->id = b->id;
	a->size = b->size;
	a->cic = b->cic;
	a->status = b->status;

	sprintf(b->name, nametmp);
	sprintf(b->fullpath, pathtmp);

	b->is_online = onlinetmp;
	b->settings = settingstmp;
	b->id = idtmp;
	b->size = sizetmp;
	b->cic = cictmp;
	b->status = statustmp; 
}

void swap_shaders(PostProcessingEffect *a, PostProcessingEffect *b) {
	char nametmp[32];
	sprintf(nametmp, a->name);
	bool compiledtmp = a->compiled;
	
	sprintf(a->name, b->name);
	a->compiled = b->compiled;
	
	sprintf(b->name, nametmp);
	b->compiled = compiledtmp;
}

void swap_overlays(Overlay *a, Overlay *b) {
	char nametmp[32];
	sprintf(nametmp, a->name);
	sprintf(a->name, b->name);
	sprintf(b->name, nametmp);
}

void sort_romlist(RomSelection *start, int order) { 
	int swapped, i; 
	RomSelection *ptr1; 
	RomSelection *lptr = NULL; 
  
	/* Checking for empty list */
	if (start == NULL) 
		return; 
  
	do { 
		swapped = 0; 
		ptr1 = start; 
  
		while (ptr1->next != lptr) {
			switch (order) {
			case SORT_Z_TO_A:
				{
					if (strcasecmp(ptr1->name,ptr1->next->name) < 0) {  
						swap_roms(ptr1, ptr1->next); 
						swapped = 1; 
					}
				}
				break;
			case SORT_A_TO_Z:
				{
					if (strcasecmp(ptr1->name,ptr1->next->name) > 0) {  
						swap_roms(ptr1, ptr1->next); 
						swapped = 1; 
					}
				}
				break;
			default:
				break;
			}
			ptr1 = ptr1->next; 
		} 
		lptr = ptr1; 
	} while (swapped); 
}

void sort_shaderlist(PostProcessingEffect *start) {
	int swapped, i; 
	PostProcessingEffect *ptr1; 
	PostProcessingEffect *lptr = NULL;
	
	/* Checking for empty list */
	if (start == NULL) 
		return; 
	
	do {
		swapped = 0;
		ptr1 = start;
		
		while (ptr1->next != lptr) {
			if (strcasecmp(ptr1->name,ptr1->next->name) > 0) {  
				swap_shaders(ptr1, ptr1->next); 
				swapped = 1; 
			}
			ptr1 = ptr1->next; 
		} 
		lptr = ptr1;
	} while (swapped);
}

void sort_overlaylist(Overlay *start) {
	int swapped, i; 
	Overlay *ptr1; 
	Overlay *lptr = NULL;
	
	/* Checking for empty list */
	if (start == NULL) 
		return; 
	
	do {
		swapped = 0;
		ptr1 = start;
		
		while (ptr1->next != lptr) {
			if (strcasecmp(ptr1->name,ptr1->next->name) > 0) {  
				swap_overlays(ptr1, ptr1->next); 
				swapped = 1; 
			}
			ptr1 = ptr1->next; 
		} 
		lptr = ptr1;
	} while (swapped);
}

bool LoadPreview(RomSelection *rom) {
	if (old_hovered == rom) return has_preview_icon;
	old_hovered = rom;
	
	IO::Filename preview_filename;
	IO::Path::Combine(preview_filename, DAEDALUS_VITA_PATH("Resources/Preview/"), rom->settings.Preview.c_str() );
	uint8_t *icon_data = stbi_load(preview_filename, &preview_width, &preview_height, NULL, 4);
	if (icon_data) {
		if (!preview_icon) glGenTextures(1, &preview_icon);
		glBindTexture(GL_TEXTURE_2D, preview_icon);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, preview_width, preview_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, icon_data);
		f32 scale = MIN(PREVIEW_WIDTH / (f32)preview_width, PREVIEW_HEIGHT / (f32)preview_height);
		preview_width = scale * (f32)preview_width;
		preview_height = scale * (f32)preview_height;
		preview_x = (PREVIEW_WIDTH - preview_width) / 2;
		preview_y = (PREVIEW_HEIGHT - preview_height) / 2;
		free(icon_data);
		return true;
	}
	
	return false;
}

// TODO: Use a proper json lib for more safety
void AppendCompatibilityDatabase(const char *file) {
	FILE *f = fopen(file, "rb");
	if (f) {
		fseek(f, 0, SEEK_END);
		uint64_t len = ftell(f);
		fseek(f, 0, SEEK_SET);
		char *buffer = (char*)malloc(len + 1);
		fread(buffer, 1, len, f);
		buffer[len] = 0;
		char *ptr = buffer;
		char *end;
		do {
			ptr = strstr(ptr, "\"title\":");
			if (ptr) {
				CompatibilityList *node = (CompatibilityList*)malloc(sizeof(CompatibilityList));
				
				// Extracting title
				ptr += 10;
				end = strstr(ptr, "\"");
				memcpy_neon(node->name, ptr, end - ptr);
				node->name[end - ptr] = 0;
				
				// Extracting tags
				bool perform_slow_check = true;
				ptr += 1000; // Let's skip some data to improve performances
				ptr = strstr(ptr, "\"labels\":");
				ptr = strstr(ptr + 150, "\"name\":");
				ptr += 9;
				if (ptr[0] == 'P') {
					node->playable = true;
					node->ingame_low = false;
					node->ingame_plus = false;
					node->crash = false;
				} else if (ptr[0] == 'C') {
					node->playable = false;
					node->ingame_low = false;
					node->ingame_plus = false;
					node->slow = false;
					node->crash = true;
					perform_slow_check = false;
				} else {
					node->playable = false;
					node->crash = false;
					end = strstr(ptr, "\"");
					if ((end - ptr) == 13) {
						node->ingame_plus = true;
						node->ingame_low = false;
					}else {
						node->ingame_low = true;
						node->ingame_plus = false;
					}
				}
				ptr += 120; // Let's skip some data to improve performances
				if (perform_slow_check) {
					end = ptr;
					ptr = strstr(ptr, "]");
					if ((ptr - end) > 200) node->slow = true;
					else node->slow = false;
				}
				
				ptr += 350; // Let's skip some data to improve performances
				node->next = comp;
				comp = node;
			}
		} while (ptr);
		fclose(f);
		free(buffer);
	}
}

CompatibilityList *SearchForCompatibilityData(const char *name) {
	CompatibilityList *node = comp;
	char tmp[128];
	sprintf(tmp, name);
	stripGameName(tmp);
	while (node) {
		if (strcasecmp(node->name, tmp) == 0) return node;
		node = node->next;
	}
	return nullptr;
}

void SetTagDescription(const char *text) {
	ImGui::SameLine();
	ImGui::TextWrapped(": %s", text);
}

bool filterRoms(RomSelection *p) {
	if (!p->status) return filter_idx != FILTER_NO_TAGS;
	else {
		if (filter_idx == FILTER_NO_TAGS) return true;
		else if ((!p->status->playable && filter_idx == FILTER_PLAYABLE) ||
			(!p->status->ingame_plus && filter_idx == FILTER_INGAME_PLUS) ||
			(!p->status->ingame_low && filter_idx == FILTER_INGAME_MINUS) ||
			(!p->status->crash && filter_idx == FILTER_CRASH)) {
			return true;
		}
	}
	return false;
}

char *DrawRomSelector() {
	bool selected = false;
	
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	vglStartRendering();
	DrawMenuBar();
		
	if (!list) {
		oldSortOrder = -1;
		
		if (!comp) {
			for (int i = 1; i <= NUM_DB_CHUNKS; i++) {
				char dbname[64];
				sprintf(dbname, "%sdb%ld.json", DAEDALUS_VITA_MAIN_PATH, i);
				AppendCompatibilityDatabase(dbname);
			}
		}
		std::string			full_path;

		IO::FindHandleT		find_handle;
		IO::FindDataT		find_data;
		
		const char *rom_folders[ROMS_FOLDERS_NUM] = {
			DAEDALUS_VITA_PATH_EXT("ux0:" , "Roms/"),
			DAEDALUS_VITA_PATH_EXT("uma0:", "Roms/"),
			DAEDALUS_PSP_PATH_EXT("ux0:" , "Roms/"),
			DAEDALUS_PSP_PATH_EXT("uma0:", "Roms/"),
			gCustomRomPath
		};
		
		for (int i = 0; i < ROMS_FOLDERS_NUM; i++) {
			if(IO::FindFileOpen( rom_folders[i], &find_handle, find_data ))
			{
				do
				{
					const char * rom_filename( find_data.Name );
					if(IsRomfilename( rom_filename ))
					{
						std::string full_path = rom_folders[i];
						full_path += rom_filename;
						RomSelection *node = (RomSelection*)malloc(sizeof(RomSelection));
						node->is_online = false;
						sprintf(node->name, rom_filename);
						sprintf(node->fullpath, full_path.c_str());
						if (ROM_GetRomDetailsByFilename(full_path.c_str(), &node->id, &node->size, &node->cic)) {
							node->size = node->size / (1024 * 1024);
							if (!CRomSettingsDB::Get()->GetSettings(node->id, &node->settings )) {
								node->settings.Reset();
								std::string game_name;
								if (!ROM_GetRomName(full_path.c_str(), game_name )) game_name = full_path;
								game_name = game_name.substr(0, 63);
								node->settings.GameName = game_name.c_str();
								CRomSettingsDB::Get()->SetSettings(node->id, node->settings);
							}
							node->status = SearchForCompatibilityData(node->settings.GameName.c_str());
						} else node->settings.GameName = lang_strings[STR_UNKNOWN];
						node->next = list;
						list = node;
					}
				}
				while(IO::FindFileNext( find_handle, find_data ));

				IO::FindFileClose( find_handle );
			}
		}
		
		if (gHasOnlineRomList) {
			char *p = (char*)rom_mem_buffer;
			while (p) {
				char *r = strstr(p, ".v64");
				if (r) {
					RomSelection *node = (RomSelection*)malloc(sizeof(RomSelection));
					node->is_online = true;
					char *space = strstr(p, " ");
					char *last_space = nullptr;
					while (space) {
						last_space = space;
						space = strstr(space + 1, " ");
					}
					memcpy_neon(node->name, &p[49], (r - &p[49]) + 4);
					node->name[(r - last_space) + 3] = 0;
					node->next = list;
					list = node;
				} else break;
				p = r + 1;
			}
		}
	}
	
	if (oldSortOrder != gSortOrder) {
		oldSortOrder = gSortOrder;
		sort_romlist(list, gSortOrder);
	}

	ImGui::SetNextWindowPos(ImVec2(0, 19 * UI_SCALE), ImGuiSetCond_Always);
	ImGui::SetNextWindowSize(ImVec2(SCR_WIDTH - 400, SCR_HEIGHT - 19 * UI_SCALE), ImGuiSetCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::Begin("Selector Window", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
	RomSelection *hovered = nullptr;
	RomSelection *p = list;
	
	ImGui::AlignTextToFramePadding();
	ImGui::Text(lang_strings[STR_SEARCH]);
	ImGui::SameLine();
	ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
	if (ImGui::Button(rom_name_filter, ImVec2(400.0f, 22.0f * UI_SCALE))) {
		showDialog(lang_strings[STR_DLG_SEARCH_ROM], apply_rom_name_filter, dummy_func, DIALOG_KEYBOARD, rom_name_filter);
	}
	ImGui::PopStyleVar();
	ImGui::AlignTextToFramePadding();
	ImGui::Text(lang_strings[STR_FILTER_BY]);
	ImGui::SameLine();
	if (ImGui::BeginCombo("##combo", filter_modes[filter_idx])) {
		for (int n = 0; n < FILTER_MODES_NUM; n++) {
			bool is_selected = filter_idx == n;
			if (ImGui::Selectable(filter_modes[n], is_selected))
				filter_idx = n;
			if (is_selected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	ImGui::Separator();
	
	if (strlen(rom_name_filter) > 0) { // Filter results with searchbox
		while (p) {
			if (strcasestr(p->name, rom_name_filter)) {
				if (filter_idx > 0) { // Apply filters
					if (filterRoms(p)) {
						p = p->next;
						continue;
					}
				}
				if (ImGui::Button(p->name)){
					sprintf(selectedRom, p->fullpath);
					selected = true;
				}
				if (ImGui::IsItemHovered()) hovered = p;
			}
			p = p->next;
		}
	} else { // No filters
		while (p) {
			if (filter_idx > 0) { // Apply filters
				if (filterRoms(p)) {
					p = p->next;
					continue;
				}
			}
			if (p->is_online) {
				ImGui::PushStyleColor(ImGuiCol_Button, 0x880015FF);
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, 0xFF0000FF);
			}
			if (ImGui::Button(p->name)){
				sprintf(selectedRom, p->fullpath);
				selected = true;
			}
			if (p->is_online) {
				ImGui::PopStyleColor(2);
			}
			if (ImGui::IsItemHovered()) hovered = p;
			p = p->next;
		}
	}

	ImGui::End();
	
	ImGui::SetNextWindowPos(ImVec2(SCR_WIDTH - 400, 19 * UI_SCALE), ImGuiSetCond_Always);
	ImGui::SetNextWindowSize(ImVec2(400, SCR_HEIGHT - 19 * UI_SCALE), ImGuiSetCond_Always);
	ImGui::Begin("Info Window", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
	if (hovered) {
		if (hovered->is_online) {
			ImGui::Text("No info available for net roms.");
		} else {
			if (has_preview_icon = LoadPreview(hovered)) {
				ImGui::SetCursorPos(ImVec2(preview_x + PREVIEW_PADDING, preview_y + PREVIEW_PADDING));
				ImGui::Image((void*)preview_icon, ImVec2(preview_width, preview_height));
			}
			ImGui::Text("%s: %s", lang_strings[STR_GAME_NAME], hovered->settings.GameName.c_str());
			ImGui::Text("%s: %s", lang_strings[STR_REGION], ROM_GetCountryNameFromID(hovered->id.CountryID));
			ImGui::Text("CRC: %04x%04x-%01x", hovered->id.CRC[0], hovered->id.CRC[1], hovered->id.CountryID);
			ImGui::Text("%s: %s", lang_strings[STR_CIC_TYPE], ROM_GetCicName(hovered->cic));
			ImGui::Text("%s: %lu MBs", lang_strings[STR_ROM_SIZE], hovered->size);
			ImGui::Text("%s: %s", lang_strings[STR_SAVE_TYPE], ROM_GetSaveTypeName(hovered->settings.SaveType));
			if (hovered->status) {
				ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10);
				if (!gBigText) ImGui::Text("%s:", lang_strings[STR_TAGS]);
				if (hovered->status->playable) {
					ImGui::TextColored(ImVec4(0, 0.75f, 0, 1.0f), "%s", lang_strings[STR_GAME_PLAYABLE]);
					if (!gBigText) SetTagDescription(lang_strings[STR_PLAYABLE_DESC]);
				}
				if (hovered->status->ingame_plus) {
					ImGui::TextColored(ImVec4(1.0f, 1.0f, 0, 1.0f), "%s", lang_strings[STR_GAME_INGAME_PLUS]);
					if (gBigText) ImGui::SameLine();
					else SetTagDescription(lang_strings[STR_INGAME_PLUS_DESC]);
				}
				if (hovered->status->ingame_low) {
					ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.25f, 1.0f), "%s", lang_strings[STR_GAME_INGAME_MINUS]);
					if (!gBigText) SetTagDescription(lang_strings[STR_INGAME_MINUS_DESC]);
				}
				if (hovered->status->crash) {
					ImGui::TextColored(ImVec4(1.0f, 0, 0, 1.0f), "%s", lang_strings[STR_GAME_CRASH]);
					if (!gBigText) SetTagDescription(lang_strings[STR_CRASH_DESC]);
				}
				if (hovered->status->slow) {
					if (gBigText) {
						ImGui::SameLine();
						ImGui::Text("&");
						ImGui::SameLine();
					}
					ImGui::TextColored(ImVec4(0.5f, 0, 1.0f, 1.0f), "%s", lang_strings[STR_GAME_SLOW]);
					if (!gBigText) SetTagDescription(lang_strings[STR_SLOW_DESC]);
				}
			}
		}
	}
	
	ImGui::End();
	ImGui::PopStyleVar();
	DrawPendingAlert();
	
	glViewport(0, 0, static_cast<int>(ImGui::GetIO().DisplaySize.x), static_cast<int>(ImGui::GetIO().DisplaySize.y));
	ImGui::Render();
	ImGui_ImplVitaGL_RenderDrawData(ImGui::GetDrawData());
	DrawPendingDialog();
	vglStopRendering();
	
	if (pendingDownload) {
		switch (cur_download.type) {
		case FILE_DOWNLOAD:
			{
				if (download_file(cur_download.url, TEMP_DOWNLOAD_NAME, cur_download.msg, cur_download.size, true) >= 0)
					cur_download.post_func();
			}
			break;
		case MEM_DOWNLOAD:
			{
				if (download_file(cur_download.url, TEMP_DOWNLOAD_NAME, cur_download.msg, cur_download.size, false) >= 0)
					cur_download.post_func();
			}
			break;
		}
		pendingDownload = false;
	}
	
	if (selected) {
		CheatCodes_Read( hovered->settings.GameName.c_str(), "Daedalus.cht", hovered->id.CountryID );
		return selectedRom;
	}
	return nullptr;
}