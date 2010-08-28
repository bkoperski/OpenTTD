/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file object_cmd.cpp Handling of object tiles. */

#include "stdafx.h"
#include "landscape.h"
#include "command_func.h"
#include "viewport_func.h"
#include "company_base.h"
#include "town.h"
#include "bridge_map.h"
#include "genworld.h"
#include "autoslope.h"
#include "functions.h"
#include "water.h"
#include "window_func.h"
#include "company_gui.h"
#include "cheat_type.h"
#include "landscape_type.h"
#include "object.h"
#include "cargopacket.h"
#include "sprite.h"
#include "core/random_func.hpp"
#include "core/pool_func.hpp"
#include "object_map.h"
#include "object_base.h"
#include "newgrf_object.h"
#include "date_func.h"

#include "table/strings.h"
#include "table/object_land.h"

ObjectPool _object_pool("Object");
INSTANTIATE_POOL_METHODS(Object)
uint16 Object::counts[NUM_OBJECTS];

/* static */ Object *Object::GetByTile(TileIndex tile)
{
	return Object::Get(GetObjectIndex(tile));
}

/** Initialize/reset the objects. */
void InitializeObjects()
{
	_object_pool.CleanPool();
	Object::ResetTypeCounts();
}

void BuildObject(ObjectType type, TileIndex tile, CompanyID owner, Town *town)
{
	const ObjectSpec *spec = ObjectSpec::Get(type);

	TileArea ta(tile, GB(spec->size, 0, 4), GB(spec->size, 4, 4));
	Object *o = new Object();
	o->location   = ta;
	o->town       = town == NULL ? CalcClosestTownFromTile(tile) : town;
	o->build_date = _date;

	assert(o->town != NULL);

	TILE_AREA_LOOP(t, ta) {
		WaterClass wc = (IsWaterTile(t) ? GetWaterClass(t) : WATER_CLASS_INVALID);
		MakeObject(t, type, owner, o->index, wc, Random());
		MarkTileDirtyByTile(t);
	}

	Object::IncTypeCount(type);
}

/**
 * Increase the animation stage of a whole structure.
 * @param tile The tile of the structure.
 */
static void IncreaseAnimationStage(TileIndex tile)
{
	TileArea ta = Object::GetByTile(tile)->location;
	TILE_AREA_LOOP(t, ta) {
		SetAnimationFrame(t, GetAnimationFrame(t) + 1);
		MarkTileDirtyByTile(t);
	}
}

/** We encode the company HQ size in the animation stage. */
#define GetCompanyHQSize GetAnimationFrame
/** We encode the company HQ size in the animation stage. */
#define IncreaseCompanyHQSize IncreaseAnimationStage

void UpdateCompanyHQ(TileIndex tile, uint score)
{
	if (tile == INVALID_TILE) return;

	byte val;
	(val = 0, score < 170) ||
	(val++, score < 350) ||
	(val++, score < 520) ||
	(val++, score < 720) ||
	(val++, true);

	while (GetCompanyHQSize(tile) < val) {
		IncreaseCompanyHQSize(tile);
	}
}

extern CommandCost CheckFlatLand(TileArea tile_area, DoCommandFlag flags);
static CommandCost ClearTile_Object(TileIndex tile, DoCommandFlag flags);

/**
 * Build an object object
 * @param tile tile where the object will be located
 * @param flags type of operation
 * @param p1 the object type to build
 * @param p2 unused
 * @param text unused
 * @return the cost of this operation or an error
 */
CommandCost CmdBuildObject(TileIndex tile, DoCommandFlag flags, uint32 p1, uint32 p2, const char *text)
{
	CommandCost cost(EXPENSES_PROPERTY);

	ObjectType type = (ObjectType)GB(p1, 0, 8);
	const ObjectSpec *spec = ObjectSpec::Get(type);
	if (!spec->IsAvailable()) return CMD_ERROR;

	if (spec->flags & OBJECT_FLAG_ONLY_IN_SCENEDIT && (_game_mode != GM_EDITOR || _current_company != OWNER_NONE)) return CMD_ERROR;
	if (spec->flags & OBJECT_FLAG_ONLY_IN_GAME && (_game_mode != GM_NORMAL || _current_company > MAX_COMPANIES)) return CMD_ERROR;

	if (!Object::CanAllocateItem()) return_cmd_error(STR_ERROR_TOO_MANY_OBJECTS);
	if (Town::GetNumItems() == 0) return_cmd_error(STR_ERROR_MUST_FOUND_TOWN_FIRST);

	int size_x = GB(spec->size, 0, 4);
	int size_y = GB(spec->size, 4, 4);
	TileArea ta(tile, size_x, size_y);

	if (type == OBJECT_OWNED_LAND) {
		/* Owned land is special as it can be placed on any slope. */
		cost.AddCost(DoCommand(tile, 0, 0, flags, CMD_LANDSCAPE_CLEAR));
	} else {
		cost.AddCost(CheckFlatLand(ta, flags));
	}
	if (cost.Failed()) return cost;

	int hq_score = 0;
	switch (type) {
		case OBJECT_TRANSMITTER:
		case OBJECT_LIGHTHOUSE:
			if (GetTileSlope(tile, NULL) != SLOPE_FLAT) return_cmd_error(STR_ERROR_FLAT_LAND_REQUIRED);
			break;

		case OBJECT_OWNED_LAND:
			if (IsTileType(tile, MP_OBJECT) &&
					IsTileOwner(tile, _current_company) &&
					IsOwnedLand(tile)) {
				return_cmd_error(STR_ERROR_YOU_ALREADY_OWN_IT);
			}
			break;

		case OBJECT_HQ: {
			Company *c = Company::Get(_current_company);
			if (c->location_of_HQ != INVALID_TILE) {
				/* We need to persuade a bit harder to remove the old HQ. */
				_current_company = OWNER_WATER;
				cost.AddCost(ClearTile_Object(c->location_of_HQ, flags));
				_current_company = c->index;
			}

			if (flags & DC_EXEC) {
				hq_score = UpdateCompanyRatingAndValue(c, false);
				c->location_of_HQ = tile;
				SetWindowDirty(WC_COMPANY, c->index);
			}
			break;
		}

		default: break;
	}

	if (flags & DC_EXEC) {
		BuildObject(type, tile, _current_company);

		/* Make sure the HQ starts at the right size. */
		if (type == OBJECT_HQ) UpdateCompanyHQ(tile, hq_score);
	}

	cost.AddCost(ObjectSpec::Get(type)->GetBuildCost() * size_x * size_y);
	return cost;
}


static Foundation GetFoundation_Object(TileIndex tile, Slope tileh);

static void DrawTile_Object(TileInfo *ti)
{
	ObjectType type = GetObjectType(ti->tile);
	const ObjectSpec *spec = ObjectSpec::Get(type);

	/* Fall back for when the object doesn't exist anymore. */
	if (!spec->enabled) type = OBJECT_TRANSMITTER;

	if ((spec->flags & OBJECT_FLAG_HAS_NO_FOUNDATION) == 0) DrawFoundation(ti, GetFoundation_Object(ti->tile, ti->tileh));

	const DrawTileSprites *dts = NULL;
	Owner to = GetTileOwner(ti->tile);
	PaletteID palette = to == OWNER_NONE ? PAL_NONE : COMPANY_SPRITE_COLOUR(to);

	if (type == OBJECT_HQ) {
		TileIndex diff = ti->tile - Object::GetByTile(ti->tile)->location.tile;
		dts = &_object_hq[GetCompanyHQSize(ti->tile) << 2 | TileY(diff) << 1 | TileX(diff)];
	} else {
		dts = &_objects[type];
	}

	if (spec->flags & OBJECT_FLAG_HAS_NO_FOUNDATION) {
		/* If an object has no foundation, but tries to draw a (flat) ground
		 * type... we have to be nice and convert that for them. */
		switch (dts->ground.sprite) {
			case SPR_FLAT_BARE_LAND:          DrawClearLandTile(ti, 0); break;
			case SPR_FLAT_1_THIRD_GRASS_TILE: DrawClearLandTile(ti, 1); break;
			case SPR_FLAT_2_THIRD_GRASS_TILE: DrawClearLandTile(ti, 2); break;
			case SPR_FLAT_GRASS_TILE:         DrawClearLandTile(ti, 3); break;
			default: DrawGroundSprite(dts->ground.sprite, palette);     break;
		}
	} else {
		DrawGroundSprite(dts->ground.sprite, palette);
	}

	if (!IsInvisibilitySet(TO_STRUCTURES)) {
		const DrawTileSeqStruct *dtss;
		foreach_draw_tile_seq(dtss, dts->seq) {
			AddSortableSpriteToDraw(
				dtss->image.sprite, palette,
				ti->x + dtss->delta_x, ti->y + dtss->delta_y,
				dtss->size_x, dtss->size_y,
				dtss->size_z, ti->z + dtss->delta_z,
				IsTransparencySet(TO_STRUCTURES)
			);
		}
	}

	if (spec->flags & OBJECT_FLAG_ALLOW_UNDER_BRIDGE) DrawBridgeMiddle(ti);
}

static uint GetSlopeZ_Object(TileIndex tile, uint x, uint y)
{
	if (IsOwnedLand(tile)) {
		uint z;
		Slope tileh = GetTileSlope(tile, &z);

		return z + GetPartialZ(x & 0xF, y & 0xF, tileh);
	} else {
		return GetTileMaxZ(tile);
	}
}

static Foundation GetFoundation_Object(TileIndex tile, Slope tileh)
{
	return IsOwnedLand(tile) ? FOUNDATION_NONE : FlatteningFoundation(tileh);
}

static CommandCost ClearTile_Object(TileIndex tile, DoCommandFlag flags)
{
	ObjectType type = GetObjectType(tile);
	const ObjectSpec *spec = ObjectSpec::Get(type);

	/* Get to the northern most tile. */
	Object *o = Object::GetByTile(tile);
	TileArea ta = o->location;

	/* Water can remove everything! */
	if (_current_company != OWNER_WATER) {
		if ((spec->flags & OBJECT_FLAG_AUTOREMOVE) == 0 && flags & DC_AUTO) {
			/* No automatic removal by overbuilding stuff. */
			return_cmd_error(type == OBJECT_HQ ? STR_ERROR_COMPANY_HEADQUARTERS_IN : STR_ERROR_OBJECT_IN_THE_WAY);
		} else if (_game_mode == GM_EDITOR) {
			/* No further limitations for the editor. */
		} else if (GetTileOwner(tile) == OWNER_NONE) {
			/* Owned by nobody, so we can only remove it with brute force! */
			if (!_cheats.magic_bulldozer.value) return CMD_ERROR;
		} else if (CheckTileOwnership(tile).Failed()) {
			/* We don't own it!. */
			return_cmd_error(STR_ERROR_OWNED_BY);
		} else if ((spec->flags & OBJECT_FLAG_AUTOREMOVE) == 0 && !_cheats.magic_bulldozer.value) {
			/* In the game editor or with cheats we can remove, otherwise we can't. */
			return CMD_ERROR;
		}
	}

	CommandCost cost(EXPENSES_CONSTRUCTION, spec->GetClearCost() * ta.w * ta.h);
	if (spec->flags & OBJECT_FLAG_CLEAR_INCOME) cost.MultiplyCost(-1); // They get an income!

	switch (type) {
		case OBJECT_HQ: {
			Company *c = Company::Get(GetTileOwner(tile));
			if (flags & DC_EXEC) {
				c->location_of_HQ = INVALID_TILE; // reset HQ position
				SetWindowDirty(WC_COMPANY, c->index);
				CargoPacket::InvalidateAllFrom(ST_HEADQUARTERS, c->index);
			}

			/* cost of relocating company is 1% of company value */
			cost = CommandCost(EXPENSES_PROPERTY, CalculateCompanyValue(c) / 100);
			break;
		}

		case OBJECT_STATUE:
			if (flags & DC_EXEC) {
				Town *town = o->town;
				ClrBit(town->statues, GetTileOwner(tile));
				SetWindowDirty(WC_TOWN_AUTHORITY, town->index);
			}
			break;

		default:
			break;
	}

	if (flags & DC_EXEC) {
		Object::DecTypeCount(type);
		TILE_AREA_LOOP(tile_cur, ta) MakeWaterKeepingClass(tile_cur, GetTileOwner(tile_cur));
		delete o;
	}

	return cost;
}

static void AddAcceptedCargo_Object(TileIndex tile, CargoArray &acceptance, uint32 *always_accepted)
{
	if (!IsCompanyHQ(tile)) return;

	/* HQ accepts passenger and mail; but we have to divide the values
	 * between 4 tiles it occupies! */

	/* HQ level (depends on company performance) in the range 1..5. */
	uint level = GetCompanyHQSize(tile) + 1;

	/* Top town building generates 10, so to make HQ interesting, the top
	 * type makes 20. */
	acceptance[CT_PASSENGERS] += max(1U, level);
	SetBit(*always_accepted, CT_PASSENGERS);

	/* Top town building generates 4, HQ can make up to 8. The
	 * proportion passengers:mail is different because such a huge
	 * commercial building generates unusually high amount of mail
	 * correspondence per physical visitor. */
	acceptance[CT_MAIL] += max(1U, level / 2);
	SetBit(*always_accepted, CT_MAIL);
}


static void GetTileDesc_Object(TileIndex tile, TileDesc *td)
{
	td->str = ObjectSpec::GetByTile(tile)->name;
	td->owner[0] = GetTileOwner(tile);
	td->build_date = Object::GetByTile(tile)->build_date;
}

static void TileLoop_Object(TileIndex tile)
{
	if (IsTileOnWater(tile)) TileLoop_Water(tile);

	if (!IsCompanyHQ(tile)) return;

	/* HQ accepts passenger and mail; but we have to divide the values
	 * between 4 tiles it occupies! */

	/* HQ level (depends on company performance) in the range 1..5. */
	uint level = GetCompanyHQSize(tile) + 1;
	assert(level < 6);

	StationFinder stations(TileArea(tile, 2, 2));

	uint r = Random();
	/* Top town buildings generate 250, so the top HQ type makes 256. */
	if (GB(r, 0, 8) < (256 / 4 / (6 - level))) {
		uint amt = GB(r, 0, 8) / 8 / 4 + 1;
		if (_economy.fluct <= 0) amt = (amt + 1) >> 1;
		MoveGoodsToStation(CT_PASSENGERS, amt, ST_HEADQUARTERS, GetTileOwner(tile), stations.GetStations());
	}

	/* Top town building generates 90, HQ can make up to 196. The
	 * proportion passengers:mail is about the same as in the acceptance
	 * equations. */
	if (GB(r, 8, 8) < (196 / 4 / (6 - level))) {
		uint amt = GB(r, 8, 8) / 8 / 4 + 1;
		if (_economy.fluct <= 0) amt = (amt + 1) >> 1;
		MoveGoodsToStation(CT_MAIL, amt, ST_HEADQUARTERS, GetTileOwner(tile), stations.GetStations());
	}
}


static TrackStatus GetTileTrackStatus_Object(TileIndex tile, TransportType mode, uint sub_mode, DiagDirection side)
{
	return 0;
}

static bool ClickTile_Object(TileIndex tile)
{
	if (!IsCompanyHQ(tile)) return false;

	ShowCompany(GetTileOwner(tile));
	return true;
}


/* checks, if a radio tower is within a 9x9 tile square around tile */
static bool IsRadioTowerNearby(TileIndex tile)
{
	TileIndex tile_s = tile - TileDiffXY(min(TileX(tile), 4U), min(TileY(tile), 4U));
	uint w = min(TileX(tile), 4U) + 1 + min(MapMaxX() - TileX(tile), 4U);
	uint h = min(TileY(tile), 4U) + 1 + min(MapMaxY() - TileY(tile), 4U);

	TILE_LOOP(tile, w, h, tile_s) {
		if (IsTransmitterTile(tile)) return true;
	}

	return false;
}

void GenerateObjects()
{
	if (_settings_game.game_creation.landscape == LT_TOYLAND) return;

	/* add radio tower */
	int radiotower_to_build = ScaleByMapSize(15); // maximum number of radio towers on the map
	int lighthouses_to_build = _settings_game.game_creation.landscape == LT_TROPIC ? 0 : ScaleByMapSize1D((Random() & 3) + 7);

	/* Scale the amount of lighthouses with the amount of land at the borders. */
	if (_settings_game.construction.freeform_edges && lighthouses_to_build != 0) {
		uint num_water_tiles = 0;
		for (uint x = 0; x < MapMaxX(); x++) {
			if (IsTileType(TileXY(x, 1), MP_WATER)) num_water_tiles++;
			if (IsTileType(TileXY(x, MapMaxY() - 1), MP_WATER)) num_water_tiles++;
		}
		for (uint y = 1; y < MapMaxY() - 1; y++) {
			if (IsTileType(TileXY(1, y), MP_WATER)) num_water_tiles++;
			if (IsTileType(TileXY(MapMaxX() - 1, y), MP_WATER)) num_water_tiles++;
		}
		/* The -6 is because the top borders are MP_VOID (-2) and all corners
		 * are counted twice (-4). */
		lighthouses_to_build = lighthouses_to_build * num_water_tiles / (2 * MapMaxY() + 2 * MapMaxX() - 6);
	}

	SetGeneratingWorldProgress(GWP_OBJECT, radiotower_to_build + lighthouses_to_build);

	for (uint i = ScaleByMapSize(1000); i != 0; i--) {
		TileIndex tile = RandomTile();

		uint h;
		if (IsTileType(tile, MP_CLEAR) && GetTileSlope(tile, &h) == SLOPE_FLAT && h >= TILE_HEIGHT * 4 && !IsBridgeAbove(tile)) {
			if (IsRadioTowerNearby(tile)) continue;

			BuildObject(OBJECT_TRANSMITTER, tile);
			IncreaseGeneratingWorldProgress(GWP_OBJECT);
			if (--radiotower_to_build == 0) break;
		}
	}

	/* add lighthouses */
	uint maxx = MapMaxX();
	uint maxy = MapMaxY();
	for (int loop_count = 0; loop_count < 1000 && lighthouses_to_build != 0; loop_count++) {
		uint r = Random();

		/* Scatter the lighthouses more evenly around the perimeter */
		int perimeter = (GB(r, 16, 16) % (2 * (maxx + maxy))) - maxy;
		DiagDirection dir;
		for (dir = DIAGDIR_NE; perimeter > 0; dir++) {
			perimeter -= (DiagDirToAxis(dir) == AXIS_X) ? maxx : maxy;
		}

		TileIndex tile;
		switch (dir) {
			default:
			case DIAGDIR_NE: tile = TileXY(maxx - 1, r % maxy); break;
			case DIAGDIR_SE: tile = TileXY(r % maxx, 1); break;
			case DIAGDIR_SW: tile = TileXY(1,        r % maxy); break;
			case DIAGDIR_NW: tile = TileXY(r % maxx, maxy - 1); break;
		}

		/* Only build lighthouses at tiles where the border is sea. */
		if (!IsTileType(tile, MP_WATER)) continue;

		for (int j = 0; j < 19; j++) {
			uint h;
			if (IsTileType(tile, MP_CLEAR) && GetTileSlope(tile, &h) == SLOPE_FLAT && h <= TILE_HEIGHT * 2 && !IsBridgeAbove(tile)) {
				BuildObject(OBJECT_LIGHTHOUSE, tile);
				IncreaseGeneratingWorldProgress(GWP_OBJECT);
				lighthouses_to_build--;
				assert(tile < MapSize());
				break;
			}
			tile = AddTileIndexDiffCWrap(tile, TileIndexDiffCByDiagDir(dir));
			if (tile == INVALID_TILE) break;
		}
	}
}

static void ChangeTileOwner_Object(TileIndex tile, Owner old_owner, Owner new_owner)
{
	if (!IsTileOwner(tile, old_owner)) return;

	if (IsOwnedLand(tile) && new_owner != INVALID_OWNER) {
		SetTileOwner(tile, new_owner);
	} else if (IsStatueTile(tile)) {
		Town *t = Object::GetByTile(tile)->town;
		ClrBit(t->statues, old_owner);
		if (new_owner != INVALID_OWNER && !HasBit(t->statues, new_owner)) {
			/* Transfer ownership to the new company */
			SetBit(t->statues, new_owner);
			SetTileOwner(tile, new_owner);
		} else {
			DoClearSquare(tile);
		}

		SetWindowDirty(WC_TOWN_AUTHORITY, t->index);
	} else {
		DoClearSquare(tile);
	}
}

static CommandCost TerraformTile_Object(TileIndex tile, DoCommandFlag flags, uint z_new, Slope tileh_new)
{
	ObjectType type = GetObjectType(tile);

	if (type == OBJECT_OWNED_LAND) {
		/* Owned land remains unsold */
		CommandCost ret = CheckTileOwnership(tile);
		if (ret.Succeeded()) return CommandCost();
	} else if (AutoslopeEnabled() && type != OBJECT_TRANSMITTER && type != OBJECT_LIGHTHOUSE) {
		if (!IsSteepSlope(tileh_new) && (z_new + GetSlopeMaxZ(tileh_new) == GetTileMaxZ(tile))) return CommandCost(EXPENSES_CONSTRUCTION, _price[PR_BUILD_FOUNDATION]);
	}

	return DoCommand(tile, 0, 0, flags, CMD_LANDSCAPE_CLEAR);
}

extern const TileTypeProcs _tile_type_object_procs = {
	DrawTile_Object,             // draw_tile_proc
	GetSlopeZ_Object,            // get_slope_z_proc
	ClearTile_Object,            // clear_tile_proc
	AddAcceptedCargo_Object,     // add_accepted_cargo_proc
	GetTileDesc_Object,          // get_tile_desc_proc
	GetTileTrackStatus_Object,   // get_tile_track_status_proc
	ClickTile_Object,            // click_tile_proc
	NULL,                        // animate_tile_proc
	TileLoop_Object,             // tile_loop_clear
	ChangeTileOwner_Object,      // change_tile_owner_clear
	NULL,                        // add_produced_cargo_proc
	NULL,                        // vehicle_enter_tile_proc
	GetFoundation_Object,        // get_foundation_proc
	TerraformTile_Object,        // terraform_tile_proc
};
