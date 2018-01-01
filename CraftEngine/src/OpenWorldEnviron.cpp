
#include "PCH-rpgcraft.h"
#include "TileMapLayer.h"

#include "x-png-decode.h"
#include "x-png-encode.h"
#include "fmod-ifc.h"
#include "imgtools.h"

#include "imgui.h"

TerrainMapItem*		g_WorldMap		= nullptr;

static FmodMusic	s_music_world;
static float		s_bgm_volume =	1.0f;

enum class TerrainTileConstructId
{
	FIRST = 0,
	Solid = 0,

	Singles,
	Single_Light	= Singles,				// single tile surrounded by unfriendlies
	Single_Heavy,							// single tile surrounded by unfriendlies

	ObtuseCorners,
	Obtuse_NW		= ObtuseCorners,		// north-west
	Obtuse_NE,								// north-east
	Obtuse_SW,								// south-west
	Obtuse_SE,								// south-east

	AcuteCorners,
	Acute_NW		= AcuteCorners,			// north-west
	Acute_NE,								// north-east
	Acute_SW,								// south-west
	Acute_SE,								// south-east

	Spans,
	Span_N			= Spans,				// north
	Span_S,									// south
	Span_W,									// west
	Span_E,									// east

	LAST
};

TerrainTileConstructId _TerrainTileSet_incr(const TerrainTileConstructId& src) {
	TerrainTileConstructId result = src;
	(int&)result += 1;
	bug_on(int(result) >= int(TerrainTileConstructId::LAST));
	return result;
}

TerrainTileConstructId _TerrainTileSet_decr(const TerrainTileConstructId& src) {
	TerrainTileConstructId result = src;
	(int&)result += 1;
	bug_on(int(result) >= int(TerrainTileConstructId::LAST));
	return result;
}

TerrainTileConstructId& operator++(TerrainTileConstructId& left)      { return left = _TerrainTileSet_incr(left); }
TerrainTileConstructId  operator++(TerrainTileConstructId& left, int) { return        _TerrainTileSet_incr(left); }
TerrainTileConstructId& operator--(TerrainTileConstructId& left)      { return left = _TerrainTileSet_decr(left); }
TerrainTileConstructId  operator--(TerrainTileConstructId& left, int) { return        _TerrainTileSet_decr(left); }

static const int TerrainTileConstruct_Count = (int)TerrainTileConstructId::LAST;

static const int2 RipSrcTilePos[TerrainTileConstruct_Count];

namespace StdTileOffset
{
	static const int Empty			= 0;
	static const int Water			= 1 + (0 * TerrainTileConstruct_Count);
	static const int Sandy			= 1 + (1 * TerrainTileConstruct_Count);
	static const int Grassy			= 1 + (2 * TerrainTileConstruct_Count);
}

template< typename T, typename T2 >
inline __ai bool xClampCheck(const T& src, const T2& boundsXY) {
	return (src < boundsXY.x) || (src > boundsXY.y);
}

union TileMatchBits {
	struct {
		u8		N	: 1;
		u8		E	: 1;
		u8		S	: 1;
		u8		W	: 1;

		u8		NW	: 1;
		u8		NE	: 1;
		u8		SE	: 1;
		u8		SW	: 1;
	};

	u8		b;

	bool CheckAll() const {
		return b == 0xff;
	}
};

enum TileMatchType
{
	None	= 0,
	N		= 1,
	E		= 2,
	NE		= 3,
	S		= 4,
	NS		= 5,
	ES		= 6,
	NES		= 7,
	W		= 8,
	NW		= 9,
	EW		= 10,
	NEW		= 11,
	SW		= 12,
	NSW		= 13,
	ESW		= 14,
	NESW	= 15,
};

#if 0
// WIP - meh, not sure about this one yet.
static const TerrainTileConstructId g_TileMatchAssoc[] =
{
	// None
	,	// N
	, // E
	// NE
	, // S
	// NS
	// ES
	Span_E, // NES
	// W
	// NW
	// EW
	Span_N, // NEW
	// SW
	Span_E, // NSW
	Span_S, // ESW
	Singles, // NESW
};
#endif

// tileDecorType - for defining variety in apperance, can be unsed for now until such time we want to "pretty things up" ...
void PlaceTileWithRules(TileClass tileClass, int tileDecorType, int2 pos)
{
	auto  thisIdx		= (pos.y * WorldSizeX) + pos.x;
	int4  edgesIdx		= {
		((pos.y + -1) * WorldSizeX) + pos.x +  0,
		((pos.y +  0) * WorldSizeX) + pos.x +  1,
		((pos.y +  1) * WorldSizeX) + pos.x +  0,
		((pos.y +  0) * WorldSizeX) + pos.x + -1,
	};

	int4  cornersIdx	= {
		edgesIdx.x - 1,
		edgesIdx.x + 1,
		edgesIdx.z - 1,
		edgesIdx.z + 1,
	};

	auto& thisTile		= g_WorldMap[thisIdx];

	//  TODO details:
	//   * This probably requires modifying neighboring tiles as well.
	//   * bounds checking on edgesIdx and cornersIdx is needed!  (for now can assume out-of-bounds edges are "water")

	int2 worldBounds = { 0, (WorldSizeX * WorldSizeY) - 1 };

	TerrainMapItem outofboundsTile;

	outofboundsTile.tile_below	= StdTileOffset::Water;
	outofboundsTile.tile_above	= StdTileOffset::Empty;
	outofboundsTile.class_below = TileClass::Water;
	outofboundsTile.class_above = TileClass::Empty;

	// Gloriously inefficient and entirely effective world bounds checking.
	// Anything out of bounds becomes a water tile for matching purposes.
	// NESW - north, east, south, west.

	auto& edgeN		= xClampCheck(edgesIdx.x,	worldBounds) ? g_WorldMap[edgesIdx.x]	: outofboundsTile;
	auto& edgeE		= xClampCheck(edgesIdx.y,	worldBounds) ? g_WorldMap[edgesIdx.y]	: outofboundsTile;
	auto& edgeS		= xClampCheck(edgesIdx.z,	worldBounds) ? g_WorldMap[edgesIdx.z]	: outofboundsTile;
	auto& edgeW		= xClampCheck(edgesIdx.w,	worldBounds) ? g_WorldMap[edgesIdx.w]	: outofboundsTile;

	auto& cornerNW	= xClampCheck(cornersIdx.x, worldBounds) ? g_WorldMap[cornersIdx.x] : outofboundsTile;
	auto& cornerNE	= xClampCheck(cornersIdx.y, worldBounds) ? g_WorldMap[cornersIdx.y] : outofboundsTile;
	auto& cornerSE	= xClampCheck(cornersIdx.w, worldBounds) ? g_WorldMap[cornersIdx.z] : outofboundsTile;
	auto& cornerSW	= xClampCheck(cornersIdx.z, worldBounds) ? g_WorldMap[cornersIdx.w] : outofboundsTile;

	// edge matching algo is probably going to _pretty_ complicated.  Just sayin'.  --jstine

	TileClass				a_class		= TileClass::Empty;
	TerrainTileConstructId	a_construct	= TerrainTileConstructId::Solid;

	TileMatchBits	matched;

	matched.N  = (edgeN.class_below == tileClass) || (edgeN.class_above == tileClass);
	matched.E  = (edgeE.class_below == tileClass) || (edgeE.class_above == tileClass);
	matched.S  = (edgeS.class_below == tileClass) || (edgeS.class_above == tileClass);
	matched.W  = (edgeW.class_below == tileClass) || (edgeW.class_above == tileClass);

	matched.NW = (cornerNW.class_below == tileClass) || (edgeN.class_above == tileClass);
	matched.NE = (cornerNE.class_below == tileClass) || (edgeE.class_above == tileClass);
	matched.SE = (cornerSE.class_below == tileClass) || (edgeS.class_above == tileClass);
	matched.SW = (cornerSW.class_below == tileClass) || (edgeW.class_above == tileClass);

	if (!matched.CheckAll()) {
		// some unmatched neighboring tiles.  Nearby tiles will need to be given class_above
		// assignment in order to maintain visual consistency...

		// TODO ---
	}
}

void DigThroughTile(int2 pos)
{
	auto  digSpotIndex  = (pos.y * WorldSizeX) + pos.x;
	auto& digSpot		= g_WorldMap[digSpotIndex];

	//
}

void WorldMap_Procgen()
{
	g_WorldMap		= (TerrainMapItem*)	xRealloc(g_WorldMap,	WorldSizeX    * WorldSizeY    * sizeof(TerrainMapItem));

	// Fill map with boring grass.  or sand.

	for (int y=0; y<WorldSizeY; ++y) {
		for (int x=0; x<WorldSizeX; ++x) {
			g_WorldMap		[(y * WorldSizeX) + x].tile_below	= StdTileOffset::Water;
			g_WorldMap		[(y * WorldSizeX) + x].tile_above	= StdTileOffset::Sandy;
		}
	}

	// carve a watering hole...
	for (int y=4; y<4+8; ++y) {
		for (int x=4; x<4+8; ++x) {
			g_WorldMap		[(y * WorldSizeX) + x].tile_above  = StdTileOffset::Empty;
		}
	}

}

static const int2 T2_GrabCoords[TerrainTileConstruct_Count] = {
	{ 1, 3 },	// Solid,
	{ 1, 0 },	// Obtuse_NW
	{ 2, 0 },	// Obtuse_NE
	{ 1, 1 },	// Obtuse_SW
	{ 2, 1 },	// Obtuse_SE
	{ 0, 2 },	// Acute_NW,
	{ 2, 2 },	// Acute_NE
	{ 0, 4 },	// Acute_SW
	{ 2, 4 },	// Acute_SE
	{ 1, 2 },	// Span_N
	{ 1, 2 },	// Span_S
	{ 0, 3 },	// Span_W
	{ 2, 3 },	// Span_E
};


static void GrabTerrainSet2(TextureAtlas& atlas, const xBitmapData& pngtex, const int2& setSize, const int2& setToGrab)
{
	auto topLeft = setToGrab * setSize;
	const auto& tileSize = atlas.m_tileSizePix;

	x_png_enc pngenc;
	for(const auto& coord : T2_GrabCoords) {
		auto tl = topLeft + (coord * tileSize);
		imgtool::AddTileToAtlas(atlas, pngtex, tl);
	}
}

void OpenWorldEnviron::InitScene()
{
	if (0) {
		xBitmapData  pngtex;
		png_LoadFromFile(pngtex, ".\\rpg_maker_vx__modernrtp_tilea2_by_painhurt-d3f7rwg.png");

		// cut sets out of the source and paste them into a properly-formed TextureAtlas.

		// Assume pngtex is rpgmaker layout for now.
		// Complete Sets are 64 px wide and 96 px tall (32px set + 64px set)
		// Within those are several subsets... there's a text file describing them, search for rpgmaker.

		int2 setSize	= {64, 96};
		int2 tileSize	= {16, 16};
		auto sizeInSets = pngtex.size / setSize;

		TextureAtlas atlas;

		atlas.Init(tileSize);

		int2 setToGrab	= {5, 2};
		auto topLeft = setToGrab * setSize;
		topLeft.y += 32;	// grab the area set.


		for (int y=0; y<4; ++y, topLeft.y += 16) {
			auto tl = topLeft;
			for (int x=0; x<4; ++x, tl.x += 16) {
				imgtool::AddTileToAtlas(atlas, pngtex, tl);
			}
		}

		atlas.Solidify();

		x_png_enc pngenc;
		pngenc.WriteImage(atlas);

		pragma_todo("Create a global temp dir mount and dump things to subdirs in there...");
		xCreateDirectory("..\\tempout\\");
		pngenc.SaveImage(xFmtStr("..\\tempout\\atlas.png"));

		g_GroundLayerAbove.SetSourceTexture(atlas);
	}

	if (1) {
		xBitmapData  pngtex;
		png_LoadFromFile(pngtex, ".\\sheets\\tiles\\terrain_2.png");

		// cut sets out of the source and paste them into a properly-formed TextureAtlas.

		// terrain2 is designed a bit differently than the painhurt set:
		//   * Each tile is 32x32 pix
		//   * Each terrain set is 96x192 pixels
		//   * Sets are subdivided into four smaller sets:
		//        32x64  -- highlight decals for being placed on top of other terrain types
		//        64x64  -- obtuse turns
		//        96x96  -- acute turns and filler
		//        96x32  -- four fill tiles
		//   * In some cases the highlight decal is a single 32x64 tile, rather than two independent tiles,
		//     and might even be some special decoration unrelated to the tile set.

		int2 setSize	= {96, 192};
		int2 tileSize	= {32, 32};
		auto sizeInSets = pngtex.size / setSize;

		TextureAtlas atlas;
		atlas.Init(tileSize);

		imgtool::AddEmptyTileToAtlas(atlas);

		GrabTerrainSet2(atlas, pngtex, setSize, {6, 0});		// water
		GrabTerrainSet2(atlas, pngtex, setSize, {0, 2});		// sand
		GrabTerrainSet2(atlas, pngtex, setSize, {0, 1});		// grassy
		atlas.Solidify();

		x_png_enc pngenc;
		pngenc.WriteImage(atlas);

		xCreateDirectory("..\\tempout\\");
		pngenc.SaveImage(xFmtStr("..\\tempout\\atlas2.png"));

		g_GroundLayerAbove.SetSourceTexture(atlas);
		g_GroundLayerBelow.SetSourceTexture(atlas);
	}

	WorldMap_Procgen();

	g_GroundLayerBelow.SetDataOffsetUV(offsetof(TerrainMapItem, tile_below) / 4);
	g_GroundLayerAbove.SetDataOffsetUV(offsetof(TerrainMapItem, tile_above) / 4);

	g_GroundLayerBelow.PopulateUVs(g_WorldMap, {0,0});
	g_GroundLayerAbove.PopulateUVs(g_WorldMap, {0,0});

	fmod_CreateMusic(s_music_world, "..\\unity\\Assets\\Audio\\Music\\ff2over.s3m");
}

bool s_showLayer_above = 1;
bool s_showLayer_below = 1;

void OpenWorldEnviron::Tick()
{
	ImGui::Checkbox("Show Above-Ground Layer", &s_showLayer_above);
	ImGui::Checkbox("Show Below-Ground Layer", &s_showLayer_below);

	g_GroundLayerBelow.CenterViewOn({ g_ViewCamera.m_Eye.x, g_ViewCamera.m_Eye.y });
	g_GroundLayerAbove.CenterViewOn({ g_ViewCamera.m_Eye.x, g_ViewCamera.m_Eye.y });

	g_GroundLayerBelow.PopulateUVs(g_WorldMap);
	g_GroundLayerAbove.PopulateUVs(g_WorldMap);

	g_GroundLayerAbove.m_enableDraw = s_showLayer_above;
	g_GroundLayerBelow.m_enableDraw = s_showLayer_below;

	fmod_Play(s_music_world);
	if (ImGui::SliderFloat("BGM Volume", &s_bgm_volume, 0, 1.0f)) {
		fmod_SetVolume(s_music_world, s_bgm_volume);
	}
}
