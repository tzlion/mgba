/* Copyright (c) 2016 taizou
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vfame.h"
#include "memory.h"

static const char ADDRESS_REORDERING_1[16] = { 15, 14, 9, 1, 8, 10, 7, 3, 5, 11, 4, 0, 13, 12, 2, 6 };
static const char ADDRESS_REORDERING_2[16] = { 15, 7, 13, 5, 11, 6, 0, 9, 12, 2, 10, 14, 3, 1, 8, 4 };
static const char ADDRESS_REORDERING_3[16] = { 15, 0, 3, 12, 2, 4, 14, 13, 1, 8, 6, 7, 9, 5, 11, 10 };
static const char ADDRESS_REORDERING_GEORGE_1[16] = { 15, 7, 13, 1, 11, 10, 14, 9, 12, 2, 4, 0, 3, 5, 2, 6 };
static const char ADDRESS_REORDERING_GEORGE_2[16] = { 15, 14, 3, 12, 8, 4, 0, 13, 5, 11, 6, 7, 9, 1, 2, 10 };
static const char ADDRESS_REORDERING_GEORGE_3[16] = { 15, 0, 9, 5, 2, 6, 7, 3, 1, 8, 10, 14, 13, 12, 11, 4 };
static const char VALUE_REORDERING_1[8] = { 5, 4, 3, 2, 1, 0, 7, 6 };
static const char VALUE_REORDERING_2[8] = { 3, 2, 1, 0, 7, 6, 5, 4 };
static const char VALUE_REORDERING_3[8] = { 1, 0, 7, 6, 5, 4, 3, 2 };
static const char VALUE_REORDERING_GEORGE_1[8] = { 3, 0, 7, 2, 1, 4, 5, 6 };
static const char VALUE_REORDERING_GEORGE_2[8] = { 1, 4, 3, 0, 5, 6, 7, 2 };
static const char VALUE_REORDERING_GEORGE_3[8] = { 5, 2, 1, 6, 7, 0, 3, 4 };

static const int8_t MODE_CHANGE_START_SEQUENCE[5] = { 0x99, 0x02, 0x05, 0x02, 0x03 };
static const int8_t MODE_CHANGE_END_SEQUENCE[5] = { 0x99, 0x03, 0x62, 0x02, 0x56 };

// A portion of the initialisation routine that gets copied into RAM - Always seems to be present at 0x15C in VFame game ROM
static const char INIT_SEQUENCE[16] = { 0xB4, 0x00, 0x9F, 0xE5, 0x99, 0x10, 0xA0, 0xE3, 0x00, 0x10, 0xC0, 0xE5, 0xAC, 0x00, 0x9F, 0xE5 };

static bool _isInMirroredArea(uint32_t address, size_t romSize);
static uint32_t _getPatternValue(uint32_t addr);
static uint32_t _patternRightShift1(uint32_t addr);
static uint32_t _patternRightShift2(uint32_t addr);
static int8_t _modifySramValue(enum GBAVFameCartType type, int8_t value, int mode);
static uint32_t _modifySramAddress(enum GBAVFameCartType type, uint32_t address, int mode);
static int _reorderBits(uint32_t value, const char* reordering, int reorderLength);

void GBAVFameInit(struct GBAVFameCart* cart) {
	cart->cartType = VFAME_NO;
	cart->sramMode = -1;
	cart->romMode = -1;
	cart->acceptingModeChange = false;
}

void GBAVFameDetect(struct GBAVFameCart* cart, uint32_t* rom, size_t romSize) {
	cart->cartType = VFAME_NO;

	// The initialisation code is also present & run in the dumps of Digimon Ruby & Sapphire from hacked/deprotected reprint carts,
	// which would break if run in "proper" VFame mode so we need to exclude those..
	if (romSize == 0x2000000) { // the deprotected dumps are 32MB but no real VF games are this size
		return;
	}

	if (memcmp(INIT_SEQUENCE, &((char*)rom)[0x15C], 16) == 0) {
		cart->cartType = VFAME_STANDARD;
		mLOG(GBA_MEM, INFO, "Vast Fame game detected");
	}

	// This game additionally operates with a different set of SRAM modes
	// Its initialisation seems to be identical so the difference must be in the cart HW itself
	// Other undumped games may have similar differences
	if (memcmp("George Sango", &((char*)rom)[0xA0], 12) == 0) {
		cart->cartType = VFAME_GEORGE;
		mLOG(GBA_MEM, INFO, "George mode");
	}
}

uint32_t GBAVFameModifyRomAddress(struct GBAVFameCart* cart,uint32_t address, size_t romSize) {
	if (cart->romMode == -1 && address <= 0x08FFFFFF) {
		// When ROM mode is uninitialised, only the first 0x80000 bytes are readable
		// All known games set the ROM mode to 00 which enables full range of reads, it's currently unknown what other values do
		address = address & 0x7FFFF;
	} else if (_isInMirroredArea(address, romSize)) {
		address -= 0x800000;
	}
	return address;
}

static bool _isInMirroredArea(uint32_t address, size_t romSize) {
	// For some reason known 4m games e.g. Zook, Sango repeat the game at 800000 but the 8m Digimon R. does not
	return (address >= 0x08800000 && address < 0x8800000 + romSize && romSize == 0x400000);
}

// Looks like only 16-bit reads are done by games but others are possible...
uint32_t GBAVFameGetPatternValue(uint32_t address, int bits) {
	switch(bits) {
		case 8:
			if (address % 2 == 1) {
				return _getPatternValue(address) & 0xFF;
			} else {
				return (_getPatternValue(address) & 0xFF00) >> 8;
			}
		case 16:
			return _getPatternValue(address);
		case 32:
			return (_getPatternValue(address) << 2) + _getPatternValue(address+2);
	}
	return 0;
}

// when you read from a ROM location outside the actual ROM data or its mirror, it returns a value based on some 16-bit transformation of the address
// which the game relies on to run
static uint32_t _getPatternValue(uint32_t addr) {
	addr = addr & 0x1FFFFF;
	uint32_t value = 0;
	switch(addr & 0x1F0000) {
		case 0x000000:
		case 0x010000:
			value = _patternRightShift1(addr);
		break;
		case 0x020000:
			value = addr & 0xFFFF;
		break;
		case 0x030000:
			value = (addr & 0xFFFF) + 1;
		break;
		case 0x040000:
			value = 0xFFFF - (addr & 0xFFFF);
		break;
		case 0x050000:
			value = (0xFFFF - (addr & 0xFFFF)) - 1;
		break;
		case 0x060000:
			value = (addr & 0xFFFF) ^ 0xAAAA;
		break;
		case 0x070000:
			value = ((addr & 0xFFFF) ^ 0xAAAA) + 1;
		break;
		case 0x080000:
			value = (addr & 0xFFFF) ^ 0x5555;
		break;
		case 0x090000:
			value = ((addr & 0xFFFF) ^ 0x5555) - 1;
		break;
		case 0x0A0000:
		case 0x0B0000:
			value = _patternRightShift2(addr);
		break;
		case 0x0C0000:
		case 0x0D0000:
			value = 0xFFFF - _patternRightShift2(addr);
		break;
		case 0x0E0000:
		case 0x0F0000:
			value = _patternRightShift2(addr) ^ 0xAAAA;
		break;
		case 0x100000:
		case 0x110000:
			value = _patternRightShift2(addr) ^ 0x5555;
		break;
		case 0x120000:
			value = 0xFFFF - ((addr & 0xFFFF) >> 1);
		break;
		case 0x130000:
			value = 0xFFFF - ((addr & 0xFFFF) >> 1) - 0x8000;
		break;
		case 0x140000:
		case 0x150000:
			value = _patternRightShift1(addr) ^ 0xAAAA;
		break;
		case 0x160000:
		case 0x170000:
			value = _patternRightShift1(addr) ^ 0x5555;
		break;
		case 0x180000:
		case 0x190000:
			value = _patternRightShift1(addr) ^ 0xF0F0;
		break;
		case 0x1A0000:
		case 0x1B0000:
			value = _patternRightShift1(addr) ^ 0x0F0F;
		break;
		case 0x1C0000:
		case 0x1D0000:
			value = _patternRightShift1(addr) ^ 0xFF00;
		break;
		case 0x1E0000:
		case 0x1F0000:
			value = _patternRightShift1(addr) ^ 0x00FF;
		break;
	}

	return value & 0xFFFF;
}

static uint32_t _patternRightShift1(uint32_t addr) {
	addr = addr & 0x1FFFF;
	addr = addr >> 1;
	addr = addr & 0xFFFF;
	return addr;
}
static uint32_t _patternRightShift2(uint32_t addr) {
	uint32_t value = addr & 0xFFFF;
	value = value >> 2;
	value += (addr % 4 == 2) ? 0x8000 : 0;
	value += (addr & 0x10000) ? 0x4000 : 0;
	return value;
}

void GBAVFameSramWrite(struct GBAVFameCart* cart,uint32_t address, int8_t value, uint8_t* sramData) {
	// A certain sequence of writes to SRAM FFF8->FFFC can enable or disable "mode change" mode
	// Currently unknown if these writes have to be sequential, or what happens if you write different values, if anything
	if (address >= 0x0E00FFF8 && address <= 0xE00FFFC) {
		cart->writeSequence[address - 0x0E00FFF8] = value;
		if ( address == 0xE00FFFC ) {
			if (memcmp(MODE_CHANGE_START_SEQUENCE, cart->writeSequence, 5 * sizeof(int8_t))==0){
				cart->acceptingModeChange = true;
			}
			if (memcmp(MODE_CHANGE_END_SEQUENCE, cart->writeSequence, 5 * sizeof(int8_t))==0){
				cart->acceptingModeChange = false;
			}
		}
	}

	// If we are in "mode change mode" we can change either SRAM or ROM modes
	// Currently unknown if other SRAM writes in this mode should have any effect
	if (cart->acceptingModeChange) {
		if (address == 0x0E00FFFE) {
			cart->sramMode = value;
		} else if (address == 0xE00FFFD) {
			cart->romMode = value;
		}
	}

	if (cart->sramMode == -1) {
		// when SRAM mode is uninitialised you can't write to it
		return;
	}

	// if mode has been set - the address and value of the SRAM write will be modified
	address = _modifySramAddress(cart->cartType, address, cart->sramMode);
	value = _modifySramValue(cart->cartType, value, cart->sramMode);
	// these writes are mirrored
	if (address >= 0xE008000) {
		address -= 0x8000;
	}
	sramData[address & (SIZE_CART_SRAM - 1)] = value;
	sramData[(address + 0x8000) & (SIZE_CART_SRAM - 1)] = value;
}

static uint32_t _modifySramAddress(enum GBAVFameCartType type, uint32_t address, int mode) {
	switch(mode & 0xF) {
		case 0x00: case 0x04: case 0x08: case 0x0C:
			return address;
		break;
		case 0x01: case 0x05: case 0x09: case 0x0D:
			if (type == VFAME_GEORGE) {
				return _reorderBits(address, ADDRESS_REORDERING_GEORGE_1, 16);
			} else {
				return _reorderBits(address, ADDRESS_REORDERING_1, 16);
			}
		break;
		case 0x02: case 0x06: case 0x0A: case 0x0e:
			if (type == VFAME_GEORGE) {
				return _reorderBits(address, ADDRESS_REORDERING_GEORGE_2, 16);
			} else {
				return _reorderBits(address, ADDRESS_REORDERING_2, 16);
			}
		break;
		case 0x03: case 0x07: case 0x0b: case 0x0F:
			if (type == VFAME_GEORGE) {
				return _reorderBits(address, ADDRESS_REORDERING_GEORGE_3, 16);
			} else {
				return _reorderBits(address, ADDRESS_REORDERING_3, 16);
			}
		break;
	}
	return address;
}

static int8_t _modifySramValue(enum GBAVFameCartType type, int8_t value, int mode) {
	switch(mode & 0xF) {
		case 0x00: case 0x01: case 0x02: case 0x03:
			return value;
		break;
		case 0x04: case 0x05: case 0x06: case 0x07:
			if (type == VFAME_GEORGE) {
				return _reorderBits(value,VALUE_REORDERING_GEORGE_1, 8);
			} else {
				return _reorderBits(value,VALUE_REORDERING_1, 8);
			}
		break;
		case 0x08: case 0x09: case 0x0A: case 0x0B:
			if (type == VFAME_GEORGE) {
				return _reorderBits(value,VALUE_REORDERING_GEORGE_2, 8);
			} else {
				return _reorderBits(value,VALUE_REORDERING_2, 8);
			}
		break;
		case 0x0C: case 0x0D: case 0x0E: case 0x0F:
			if (type == VFAME_GEORGE) {
				return _reorderBits(value,VALUE_REORDERING_GEORGE_3, 8);
			} else {
				return _reorderBits(value,VALUE_REORDERING_3, 8);
			}
		break;
	}
	return value;
}

// Reorder bits in a byte according to the reordering given
static int _reorderBits(uint32_t value, const char* reordering, int reorderLength) {
	uint32_t retval = value;

	for(int x = reorderLength; x > 0; x--)
	{
		char reorderPlace = reordering[reorderLength-x]; // get the reorder position

		uint32_t mask = 1 << reorderPlace; // move the bit to the position we want
		uint32_t val = value & mask; // AND it with the original value
		val = val >> reorderPlace; // move the bit back, so we have the correct 0 or 1

		int destinationPlace = x-1;

		uint32_t newMask = 1 << destinationPlace;
		if (val == 1) {
			retval = retval | newMask;
		} else {
			retval = retval & ~newMask;
		}
	}

	return retval;
}

