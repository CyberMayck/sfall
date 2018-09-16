/*
 *    sfall
 *    Copyright (C) 2011  Timeslip
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>

#include "..\main.h"
#include "..\FalloutEngine\Fallout2.h"
#include "..\InputFuncs.h"
#include "PartyControl.h"
#include "HookScripts.h"
#include "LoadGameHook.h"

#include "Inventory.h"

namespace sfall
{

static Delegate<DWORD> onAdjustFid;

static DWORD sizeLimitMode;
static DWORD invSizeMaxLimit;
static DWORD reloadWeaponKey = 0;
static DWORD itemFastMoveKey = 0;

void InventoryKeyPressedHook(DWORD dxKey, bool pressed, DWORD vKey) {
	// TODO: move this out into a script
	if (pressed && reloadWeaponKey && dxKey == reloadWeaponKey && IsMapLoaded() && (GetLoopFlags() & ~(COMBAT | PCOMBAT)) == 0) {
		DWORD maxAmmo, curAmmo;
		fo::GameObject* item = fo::GetActiveItem();
		maxAmmo = fo::func::item_w_max_ammo(item);
		curAmmo = fo::func::item_w_curr_ammo(item);
		if (maxAmmo != curAmmo) {
			long &currentMode = fo::GetActiveItemMode();
			long previusMode = currentMode;
			currentMode = 5; // reload mode
			fo::func::intface_use_item();
			if (previusMode != 5) {
				// return to previous active item mode (if it wasn't "reload")
				currentMode = previusMode - 1;
				if (currentMode < 0) {
					currentMode = 4;
				}
				fo::func::intface_toggle_item_state();
			}
		}
	}
}

/////////////////////////////////////////////////////////////////
static DWORD __stdcall sf_item_total_size(fo::GameObject* critter) {
	int totalSize = fo::func::item_c_curr_size(critter);

	if ((critter->artFid & 0xF000000) == (fo::OBJ_TYPE_CRITTER << 24)) {
		fo::GameObject* item = fo::func::inven_right_hand(critter);
		if (item && !(item->flags & fo::ObjectFlag::Right_Hand)) {
			totalSize += fo::func::item_size(item);
		}

		fo::GameObject* itemL = fo::func::inven_left_hand(critter);
		if (itemL && item != itemL && !(itemL->flags & fo::ObjectFlag::Left_Hand)) {
			totalSize += fo::func::item_size(itemL);
		}

		item = fo::func::inven_worn(critter);
		if (item && !(item->flags & fo::ObjectFlag::Worn)) {
			totalSize += fo::func::item_size(item);
		}
	}
	return totalSize;
}

/*static const DWORD ObjPickupFail=0x49B70D;
static const DWORD ObjPickupEnd=0x49B6F8;
static const DWORD size_limit;
static __declspec(naked) void  ObjPickupHook() {
	__asm {
		cmp edi, ds:[FO_VAR_obj_dude];
		jnz end;
end:
		lea edx, [esp+0x10];
		mov eax, ecx;
		jmp ObjPickupEnd;
	}
}*/

static int __stdcall CritterGetMaxSize(fo::GameObject* critter) {
	if (critter == fo::var::obj_dude) return invSizeMaxLimit;

	if (sizeLimitMode != 3) { // selected mode 1 or 2
		if (!(sizeLimitMode & 2) || !(fo::func::isPartyMember(critter))) return 0; // if mode 2 is selected, check this party member, otherwise 0
	}

	int statSize = 0;
	fo::Proto* proto = fo::GetProto(critter->protoId);
	if (proto != nullptr) {
		statSize = proto->critter.base.unarmedDamage + proto->critter.bonus.unarmedDamage; // The unused stat in the base + extra block
	}
	return (statSize > 0) ? statSize : 100; // 100 - default value, for all critters if not set stats
}

static __declspec(naked) void critterIsOverloaded_hack() {
	__asm {
		and  eax, 0xFF;
		jnz  end;
		push ecx;
		push ebx;                // critter
		call CritterGetMaxSize;
		test eax, eax;
		jz   skip;
		push ebx;
		mov  ebx, eax;           // ebx = MaxSize
		call sf_item_total_size;
		cmp  eax, ebx;
		setg al;                 // if CurrSize > MaxSize
		and  eax, 0xFF;
skip:
		pop  ecx;
end:
		retn;
	}
}

static int __fastcall CanAddedItems(fo::GameObject* critter, fo::GameObject* item, int count) {
	int sizeMax = CritterGetMaxSize(critter);
	if (sizeMax > 0) {
		int itemsSize = fo::func::item_size(item) * count;
		if (itemsSize + (int)sf_item_total_size(critter) > sizeMax) return -6; // TODO: Switch this to a lower number, and add custom error messages.
	}
	return 0;
}

static const DWORD ItemAddMultRet  = 0x4772A6;
static const DWORD ItemAddMultFail = 0x4771C7;
static __declspec(naked) void item_add_mult_hack() {
	__asm {
		push ecx;
		push ebx;           // items count
		mov  edx, esi;      // item
		call CanAddedItems; // ecx - source;
		pop  ecx;
		test eax, eax;
		jnz  fail;
		jmp  ItemAddMultRet;
fail:
		jmp  ItemAddMultFail;
	}
}

static __declspec(naked) void item_add_mult_hack_container() {
	__asm {
		/* cmp eax, edi */
		mov  eax, -6;
		jl   fail;
		//-------
		push ecx;
		push ebx;           // items count
		mov  edx, esi;      // item
		call CanAddedItems; // ecx - source;
		pop  ecx;
		test eax, eax;
		jnz  fail;
		jmp  ItemAddMultRet;
fail:
		jmp  ItemAddMultFail;
	}
}

static int __fastcall BarterAttemptTransaction(fo::GameObject* critter, fo::GameObject* targetTable) {
	int size = CritterGetMaxSize(critter);
	if (size == 0) return 1;

	int sizeTable = sf_item_total_size(targetTable);
	if (sizeTable == 0) return 1;

	size -= sf_item_total_size(critter);
	return (sizeTable <= size) ? 1 : 0;
}

static const DWORD BarterAttemptTransactionPCFail = 0x474C81;
static const DWORD BarterAttemptTransactionPCRet  = 0x474CA8;
static __declspec(naked) void barter_attempt_transaction_hack_pc() {
	__asm {
		/* cmp  eax, edx */
		jg   fail;    // if is no free weight
		//------
		mov  ecx, edi;
		mov  edx, ebp;
		call BarterAttemptTransaction;
		test eax, eax;
		jz   fail;
		jmp  BarterAttemptTransactionPCRet;
fail:
		mov  esi, 31;
		jmp  BarterAttemptTransactionPCFail;
	}
}

static const DWORD BarterAttemptTransactionPMFail = 0x474CD8;
static const DWORD BarterAttemptTransactionPMRet  = 0x474D01;
static __declspec(naked) void barter_attempt_transaction_hack_pm() {
	__asm {
		/* cmp  eax, edx */
		jg   fail;    // if is no free weight
		//------
		mov  ecx, ebx;
		mov  edx, esi;
		call BarterAttemptTransaction;
		test eax, eax;
		jz   fail;
		jmp  BarterAttemptTransactionPMRet;
fail:
		mov  ecx, 32;
		jmp  BarterAttemptTransactionPMFail;
	}
}

static char InvenFmt[32];
static const char* InvenFmt1 = "%s %d/%d %s %d/%d";
static const char* InvenFmt2 = "%s %d/%d";
static const char* InvenFmt3 = "%d/%d | %d/%d";

static void __cdecl DisplaySizeStats(fo::GameObject* critter, const char* &message, DWORD &size, DWORD &sizeMax) {
	int limitMax = CritterGetMaxSize(critter);
	if (limitMax == 0) {
		strcpy(InvenFmt, InvenFmt2); // default fmt
		return;
	}

	sizeMax = limitMax;
	size = sf_item_total_size(critter);

	const char* msg = fo::MessageSearch(&fo::var::inventry_message_file, 35);
	message = (msg != nullptr) ? msg : "";

	strcpy(InvenFmt, InvenFmt1);
}

static const DWORD DisplayStatsRet = 0x4725E5;
static __declspec(naked) void display_stats_hack() {
	using namespace fo;
	__asm {
		mov  ecx, esp;
		sub  ecx, 4;
		push ecx;   // sizeMax
		sub  ecx, 4;
		push ecx;   // size
		sub  ecx, 4;
		push ecx;   // size message
		push eax;   // critter
		call DisplaySizeStats;
		pop  eax;
		mov  edx, STAT_carry_amt;
		jmp  DisplayStatsRet;
	}
}

static char SizeMsgBuf[32];
static const char* _stdcall SizeInfoMessage(fo::GameObject* item) {
	int size = fo::func::item_size(item);
	if (size == 1) {
		const char* message = fo::MessageSearch(&fo::var::proto_main_msg_file, 543);
		if (message == nullptr)
			strncpy_s(SizeMsgBuf, "It occupies 1 unit.", _TRUNCATE);
		else
			_snprintf_s(SizeMsgBuf, _TRUNCATE, message, size);
	} else {
		const char* message = fo::MessageSearch(&fo::var::proto_main_msg_file, 542);
		if (message == nullptr)
			_snprintf_s(SizeMsgBuf, _TRUNCATE, "It occupies %d units.", size);
		else
			_snprintf_s(SizeMsgBuf, _TRUNCATE, message, size);
	}
	return SizeMsgBuf;
}

static __declspec(naked) void inven_obj_examine_func_hook() {
	__asm {
		call fo::funcoffs::inven_display_msg_;
		push edx;
		push ecx;
		push esi;
		call SizeInfoMessage;
		pop  ecx;
		pop  edx;
		jmp  fo::funcoffs::inven_display_msg_;
	}
}

static const DWORD ControlUpdateInfoRet = 0x44912A;
static void __declspec(naked) gdControlUpdateInfo_hack() {
	using namespace fo;
	__asm {
		mov  ebx, eax;
		push eax;               // critter
		call CritterGetMaxSize;
		push eax;               // sizeMax
		push ebx;
		call sf_item_total_size;
		push eax;               // size
		mov  eax, ebx;
		mov  edx, STAT_carry_amt;
		jmp  ControlUpdateInfoRet;
	}
}
/////////////////////////////////////////////////////////////////

static std::string superStimMsg;
static int __fastcall SuperStimFix2(fo::GameObject* item, fo::GameObject* target) {
	if (item->protoId != fo::PID_SUPER_STIMPAK || !target || (target->protoId & 0xFF000000) != (fo::OBJ_TYPE_CRITTER << 24)) { // 0x01000000
		return 0;
	}

	DWORD curr_hp, max_hp;
	curr_hp = fo::func::stat_level(target, fo::STAT_current_hp);
	max_hp = fo::func::stat_level(target, fo::STAT_max_hit_points);
	if (curr_hp < max_hp) return 0;

	fo::func::display_print(superStimMsg.c_str());
	return -1;
}

static const DWORD UseItemHookRet = 0x49C5F4;
static void __declspec(naked) SuperStimFix() {
	__asm {
		push ecx;
		mov  ecx, ebx;       // ecx - item
		call SuperStimFix2;  // edx - target
		pop  ecx;
		test eax, eax;
		jnz  end;
		mov  ebp, -1;        // overwritten engine code
		retn;
end:
		add  esp, 4;         // destroy ret
		jmp  UseItemHookRet; // exit
	}
}

static int invenapcost;
static char invenapqpreduction;
void _stdcall SetInvenApCost(int a) {
	invenapcost = a;
}
static const DWORD inven_ap_cost_hook_ret = 0x46E816;
static void __declspec(naked) inven_ap_cost_hook() {
	_asm {
		movzx ebx, byte ptr invenapqpreduction;
		mul bl;
		mov edx, invenapcost;
		sub edx, eax;
		mov eax, edx;
		jmp inven_ap_cost_hook_ret;
	}
}

static DWORD __fastcall add_check_for_item_ammo_cost(register fo::GameObject* weapon, DWORD hitMode) {
	DWORD rounds = 1;
	DWORD anim = fo::func::item_w_anim_weap(weapon, hitMode);
	if (anim == fo::Animation::ANIM_fire_burst || anim == fo::Animation::ANIM_fire_continuous) {
		rounds = fo::func::item_w_rounds(weapon); // ammo in burst
	}

	if (HookScripts::IsInjectHook(HOOK_AMMOCOST)) {
		AmmoCostHook_Script(1, weapon, &rounds);  // get rounds cost from hook
	}

	DWORD currAmmo = fo::func::item_w_curr_ammo(weapon);

	DWORD cost = 1; // default cost
	if (currAmmo > 0) cost = (DWORD)ceilf((float)rounds / currAmmo);

	return (cost > currAmmo) ? 0 : 1;   // 0 - this will force "Out of ammo", 1 - this will force success (enough ammo)
}

// adds check for weapons which require more than 1 ammo for single shot (super cattle prod & mega power fist) and burst rounds
static void __declspec(naked) combat_check_bad_shot_hook() {
	__asm {
		push edx;
		push ebx;
		push ecx;         // weapon
		mov  edx, edi;    // hitMode
		call add_check_for_item_ammo_cost;
		pop  ecx;
		pop  ebx;
		pop  edx;
		retn;
	}
}

static DWORD __fastcall divide_burst_rounds_by_ammo_cost(fo::GameObject* weapon, register DWORD currAmmo, DWORD burstRounds) {
	DWORD rounds = 1; // default multiply

	if (HookScripts::IsInjectHook(HOOK_AMMOCOST)) {
		rounds = burstRounds;            // rounds in burst
		AmmoCostHook_Script(2, weapon, &rounds);
	}

	DWORD cost = burstRounds * rounds;    // so much ammo is required for this burst
	if (cost > currAmmo) cost = currAmmo; // if cost ammo more than current ammo, set it to current

	return (cost / rounds);               // divide back to get proper number of rounds for damage calculations
}

static void __declspec(naked) compute_spray_hack() {
	__asm {
		push edx;         // weapon
		push ecx;         // current ammo in weapon
		xchg ecx, edx;
		push eax;         // eax - rounds in burst attack, need to set ebp
		call divide_burst_rounds_by_ammo_cost;
		mov  ebp, eax;    // overwriten code
		pop  ecx;
		pop  edx;
		retn;
	}
}

static void __declspec(naked) SetDefaultAmmo() {
	using namespace fo;
	__asm {
		push    eax
		push    ebx
		push    edx
		xchg    eax, edx
		mov     ebx, eax
		call    fo::funcoffs::item_get_type_
		cmp     eax, item_type_weapon // is it item_type_weapon?
		jne     end // no
		cmp     dword ptr [ebx+0x3C], 0 // is there any ammo in the weapon?
		jne     end // yes
		sub     esp, 4
		mov     edx, esp
		mov     eax, [ebx+0x64] // eax = weapon pid
		call    fo::funcoffs::proto_ptr_
		mov     edx, [esp]
		mov     eax, [edx+0x5C] // eax = default ammo pid
		mov     [ebx+0x40], eax // set current ammo proto
		add     esp, 4
end:
		pop     edx
		pop     ebx
		pop     eax
		retn
	}
}

static const DWORD inven_action_cursor_hack_End = 0x4736CB;
static void __declspec(naked) inven_action_cursor_hack() {
	__asm {
		mov     edx, [esp+0x1C]
		call    SetDefaultAmmo
		cmp     dword ptr [esp+0x18], 0
		jmp     inven_action_cursor_hack_End
	}
}

static void __declspec(naked) item_add_mult_hook() {
	__asm {
		call    SetDefaultAmmo
		jmp     fo::funcoffs::item_add_force_
	}
}

static void __declspec(naked) inven_pickup_hook() {
	__asm {
		mov  eax, ds:[FO_VAR_i_wid]
		call fo::funcoffs::GNW_find_
		mov  ebx, [eax+0x8+0x0]                   // ebx = _i_wid.rect.x
		mov  ecx, [eax+0x8+0x4]                   // ecx = _i_wid.rect.y
		mov  eax, 176
		add  eax, ebx                             // x_start
		add  ebx, 176+60                          // x_end
		mov  edx, 37
		add  edx, ecx                             // y_start
		add  ecx, 37+100                          // y_end
		call fo::funcoffs::mouse_click_in_
		test eax, eax
		jz   end
		mov  edx, ds:[FO_VAR_curr_stack]
		test edx, edx
		jnz  end
		cmp  edi, 1006                            // Hands?
		jae  skip                                 // Yes
skip:
		xor  eax, eax
end:
		retn
	}
}

static void __declspec(naked) loot_container_hack2() {
	__asm {
		cmp  esi, 0x150                           // source_down
		je   scroll
		cmp  esi, 0x148                           // source_up
		jne  end
scroll:
		push edx
		push ecx
		push ebx
		mov  eax, ds:[FO_VAR_i_wid]
		call fo::funcoffs::GNW_find_
		mov  ebx, [eax+0x8+0x0]                   // ebx = _i_wid.rect.x
		mov  ecx, [eax+0x8+0x4]                   // ecx = _i_wid.rect.y
		mov  eax, 297
		add  eax, ebx                             // x_start
		add  ebx, 297+64                          // x_end
		mov  edx, 37
		add  edx, ecx                             // y_start
		add  ecx, 37+6*48                         // y_end
		call fo::funcoffs::mouse_click_in_
		pop  ebx
		pop  ecx
		pop  edx
		test eax, eax
		jz   end
		cmp  esi, 0x150                           // source_down
		je   targetDown
		mov  esi, 0x18D                           // target_up
		jmp  end
targetDown:
		mov  esi, 0x191                           // target_down
end:
		mov  eax, ds:[FO_VAR_curr_stack]
		retn
	}
}

static void __declspec(naked) barter_inventory_hack2() {
	__asm {
		push edx
		push ecx
		push ebx
		xchg esi, eax
		cmp  esi, 0x150                           // source_down
		je   scroll
		cmp  esi, 0x148                           // source_up
		jne  end
scroll:
		mov  eax, ds:[FO_VAR_i_wid]
		call fo::funcoffs::GNW_find_
		mov  ebx, [eax+0x8+0x0]                   // ebx = _i_wid.rect.x
		mov  ecx, [eax+0x8+0x4]                   // ecx = _i_wid.rect.y
		push ebx
		push ecx
		mov  eax, 395
		add  eax, ebx                             // x_start
		add  ebx, 395+64                          // x_end
		mov  edx, 35
		add  edx, ecx                             // y_start
		add  ecx, 35+3*48                         // y_end
		call fo::funcoffs::mouse_click_in_
		pop  ecx
		pop  ebx
		test eax, eax
		jz   notTargetScroll
		cmp  esi, 0x150                           // source_down
		je   targetDown
		mov  esi, 0x18D                           // target_up
		jmp  end
targetDown:
		mov  esi, 0x191                           // target_down
		jmp  end
notTargetScroll:
		push ebx
		push ecx
		mov  eax, 250
		add  eax, ebx                             // x_start
		add  ebx, 250+64                          // x_end
		mov  edx, 20
		add  edx, ecx                             // y_start
		add  ecx, 20+3*48                         // y_end
		call fo::funcoffs::mouse_click_in_
		pop  ecx
		pop  ebx
		test eax, eax
		jz   notTargetBarter
		cmp  esi, 0x150                           // source_down
		je   barterTargetDown
		mov  esi, 0x184                           // target_barter_up
		jmp  end
barterTargetDown:
		mov  esi, 0x176                           // target_barter_down
		jmp  end
notTargetBarter:
		mov  eax, 165
		add  eax, ebx                             // x_start
		add  ebx, 165+64                          // x_end
		mov  edx, 20
		add  edx, ecx                             // y_start
		add  ecx, 20+3*48                         // y_end
		call fo::funcoffs::mouse_click_in_
		test eax, eax
		jz   end
		cmp  esi, 0x150                           // source_down
		je   barterSourceDown
		mov  esi, 0x149                           // source_barter_up
		jmp  end
barterSourceDown:
		mov  esi, 0x151                           // source_barter_down
end:
		pop  ebx
		pop  ecx
		pop  edx
		mov  eax, esi
		cmp  eax, 0x11
		retn
	}
}

int __stdcall ItemCountFixStdcall(fo::GameObject* who, fo::GameObject* item) {
	int count = 0;
	for (int i = 0; i < who->invenSize; i++) {
		auto tableItem = &who->invenTable[i];
		if (tableItem->object == item) {
			count += tableItem->count;
		} else if (fo::func::item_get_type(tableItem->object) == fo::item_type_container) {
			count += ItemCountFixStdcall(tableItem->object, item);
		}
	}
	return count;
}

void __declspec(naked) ItemCountFix() {
	__asm {
		push ebx;
		push ecx;
		push edx; // save state
		push edx; // item
		push eax; // container-object
		call ItemCountFixStdcall;
		pop edx;
		pop ecx;
		pop ebx; // restore
		retn;
	}
}

// reimplementation of adjust_fid engine function
// Differences from vanilla:
// - doesn't use art_vault_guy_num as default art, uses current critter FID instead
// - invokes onAdjustFid delegate that allows to hook into FID calculation
DWORD __stdcall adjust_fid_replacement2() {
	using namespace fo;

	DWORD fid;
	if ((var::inven_dude->artFid & 0xF000000) >> 24 == OBJ_TYPE_CRITTER) {
		DWORD frameNum;
		DWORD weaponAnimCode = 0;
		if (PartyControl::IsNpcControlled()) {
			// if NPC is under control, use current FID of critter
			frameNum = var::inven_dude->artFid & 0xFFF;
		} else {
			// vanilla logic:
			frameNum = var::art_vault_guy_num;
			auto critterPro = GetProto(var::inven_pid);
			if (critterPro != nullptr) {
				frameNum = critterPro->fid & 0xFFF;
			}
			if (var::i_worn != nullptr) {
				auto armorPro = GetProto(var::i_worn->protoId);
				DWORD armorFrameNum = func::stat_level(var::inven_dude, STAT_gender) == GENDER_FEMALE
					? armorPro->item.armor.femaleFrameNum
					: armorPro->item.armor.maleFrameNum;

				if (armorFrameNum != -1) {
					frameNum = armorFrameNum;
				}
			}
		}
		auto itemInHand = func::intface_is_item_right_hand()
			? var::i_rhand
			: var::i_lhand;

		if (itemInHand != nullptr) {
			auto itemPro = GetProto(itemInHand->protoId);
			if (itemPro->item.type == item_type_weapon) {
				weaponAnimCode = itemPro->item.weapon.animationCode;
			}
		}
		fid = func::art_id(OBJ_TYPE_CRITTER, frameNum, 0, weaponAnimCode, 0);
	} else {
		fid = var::inven_dude->artFid;
	}
	var::i_fid = fid;
	onAdjustFid.invoke(fid);
	return var::i_fid;
}

void __declspec(naked) adjust_fid_replacement() {
	__asm {
		pushad;
		call adjust_fid_replacement2;
		popad;
		mov eax, [FO_VAR_i_fid];
		retn;
	}
}

void __declspec(naked) do_move_timer_hook() {
	__asm {
		cmp eax, 4;
		jnz end;
		pushad;
	}

	KeyDown(itemFastMoveKey);

	__asm {
		test eax, eax;
		popad;
		jz end;
		mov dword ptr [esp], 0x476920;
		retn;
end:
		call fo::funcoffs::setup_move_timer_win_;
		retn;
	}
}

void InventoryReset() {
	invenapcost = GetConfigInt("Misc", "InventoryApCost", 4);
}

void Inventory::init() {
	OnKeyPressed() += InventoryKeyPressedHook;
	LoadGameHook::OnGameReset() += InventoryReset;

	MakeJump(fo::funcoffs::adjust_fid_, adjust_fid_replacement);

	sizeLimitMode = GetConfigInt("Misc", "CritterInvSizeLimitMode", 0);
	if (sizeLimitMode > 0 && sizeLimitMode <= 7) {
		if (sizeLimitMode >= 4) {
			sizeLimitMode -= 4;
			SafeWrite8(0x477EB3, 0xEB);
		}
		invSizeMaxLimit = GetConfigInt("Misc", "CritterInvSizeLimit", 100);

		// Check item_add_multi (picking stuff from the floor, etc.)
		HookCall(0x4771BD, item_add_mult_hack); // jle addr
		SafeWrite16(0x47726F, 0x9090);
		MakeJump(0x477271, item_add_mult_hack_container);
		MakeCall(0x42E688, critterIsOverloaded_hack);

		// Check player's capacity when bartering
		SafeWrite16(0x474C7A, 0x9090);
		MakeJump(0x474C7C, barter_attempt_transaction_hack_pc);

		// Display total weight/size on the inventory screen
		MakeJump(0x4725E0, display_stats_hack);
		SafeWrite32(0x4725FF, (DWORD)&InvenFmt);
		SafeWrite8(0x47260F, 0x20);
		SafeWrite32(0x4725F9, 0x9C + 0x0C);
		SafeWrite8(0x472606, 0x10 + 0x0C);
		SafeWrite32(0x472632, 150); // width
		SafeWrite8(0x472638, 0);    // x offset position

		// Display item size when examining
		HookCall(0x472FFE, inven_obj_examine_func_hook);

		if (sizeLimitMode > 1) {
			// Check party member's capacity when bartering
			SafeWrite16(0x474CD1, 0x9090);
			MakeJump(0x474CD3, barter_attempt_transaction_hack_pm);

			// Display party member's current/max inventory size on the combat control panel
			MakeJump(0x449125, gdControlUpdateInfo_hack);
			SafeWrite32(0x44913E, (DWORD)InvenFmt3);
			SafeWrite8(0x449145, 0x0C + 0x08);
			SafeWrite8(0x449150, 0x10 + 0x08);
		}
	}

	invenapcost = GetConfigInt("Misc", "InventoryApCost", 4);
	invenapqpreduction = GetConfigInt("Misc", "QuickPocketsApCostReduction", 2);
	MakeJump(0x46E80B, inven_ap_cost_hook);

	if (GetConfigInt("Misc", "SuperStimExploitFix", 0)) {
		superStimMsg = Translate("sfall", "SuperStimExploitMsg", "You cannot use a super stim on someone who is not injured!");
		MakeCall(0x49C3D9, SuperStimFix);
	}

	if (GetConfigInt("Misc", "CheckWeaponAmmoCost", 0)) {
		HookCall(0x4266E9, combat_check_bad_shot_hook);
		MakeCall(0x4234B3, compute_spray_hack);
		SafeWrite8(0x4234B8, 0x90);
	}

	reloadWeaponKey = GetConfigInt("Input", "ReloadWeaponKey", 0);

	if (GetConfigInt("Misc", "StackEmptyWeapons", 0)) {
		MakeJump(0x4736C6, inven_action_cursor_hack);
		HookCall(0x4772AA, &item_add_mult_hook);
	}

	// Do not call the 'Move Items' window when using drap and drop to reload weapons in the inventory
	int ReloadReserve = GetConfigInt("Misc", "ReloadReserve", -1);
	if (ReloadReserve >= 0) {
		SafeWrite32(0x47655F, ReloadReserve);     // mov  eax, ReloadReserve
		SafeWrite32(0x476563, 0x097EC139);        // cmp  ecx, eax; jle  0x476570
		SafeWrite16(0x476567, 0xC129);            // sub  ecx, eax
		SafeWrite8(0x476569, 0x91);               // xchg ecx, eax
	};

	itemFastMoveKey = GetConfigInt("Input", "ItemFastMoveKey", DIK_LCONTROL);
	if (itemFastMoveKey > 0) {
		HookCall(0x476897, do_move_timer_hook);
	}

	if (GetConfigInt("Misc", "ItemCounterDefaultMax", 0)) {
		BlockCall(0x4768A3); // mov  ebx, 1
	}

	// Move items out of bag/backpack and back into the main inventory list by dragging them to character's image
	// (similar to Fallout 1 behavior)
	HookCall(0x471457, &inven_pickup_hook);

	// Move items to player's main inventory instead of the opened bag/backpack when confirming a trade
	SafeWrite32(0x475CF2, FO_VAR_stack);

	// Enable mouse scroll control in barter and loot screens when the cursor is hovering over other lists
	if (useScrollWheel) {
		MakeCall(0x473E66, loot_container_hack2);
		MakeCall(0x4759F1, barter_inventory_hack2);
		fo::var::max = 100;
	};

	// Fix item_count function returning incorrect value when there is a container-item inside
	MakeJump(0x47808C, ItemCountFix); // replacing item_count_ function
}

Delegate<DWORD>& Inventory::OnAdjustFid() {
	return onAdjustFid;
}



}
