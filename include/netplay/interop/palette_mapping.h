// Ported verbatim from efz_palette_override/src/palette_mapping.h (offline
// portrait-palette mod), which is itself VERIFIED against 0palrandomizer.ps1.
// Do not hand-edit the tables; re-port from the source of truth instead.
// Static-table header: include from exactly one translation unit
// (palette_remap.cpp).
/**
 * Palette Mapping Data - VERIFIED against 0palrandomizer.ps1
 * 
 * Maps sprite palette indices (.pal files) to portrait palette indices (game memory).
 * 
 * $pals = sprite indices (what to read from .pal)
 * $portraits = portrait ranges [from, to] (where to write in portrait)
 * 
 * Only areas that exist in BOTH $pals and $portraits are mapped.
 */

#pragma once

#include <cstdint>
#include <cstring>

//=============================================================================
// Structures
//=============================================================================

struct SpriteArea {
    const char* name;
    const int8_t* indices;
    int count;
};

struct PortraitArea {
    const char* name;
    int start;
    int end;
};

struct CharacterMapping {
    const char* charName;
    const SpriteArea* spriteAreas;
    int spriteAreaCount;
    const PortraitArea* portraitAreas;
    int portraitAreaCount;
};

//=============================================================================
// AKANE
// $pals: skin:[2,3,4,5], hair:[6,7,8,9,10], top:[11,12,13], bottom:[14,15,16], acc:[23,24,25]
// $portraits: skin:[2,8], hair:[32,40], top:[14,20], bottom:[25,30], acc:[13,9]
//=============================================================================
static const int8_t akane_spr_skin[] = {2,3,4,5};
static const int8_t akane_spr_hair[] = {6,7,8,9,10};
static const int8_t akane_spr_top[] = {11,12,13};
static const int8_t akane_spr_bottom[] = {14,15,16};
static const int8_t akane_spr_acc[] = {23,24,25};

static const SpriteArea akane_sprite[] = {
    {"skin", akane_spr_skin, 4},
    {"hair", akane_spr_hair, 5},
    {"top", akane_spr_top, 3},
    {"bottom", akane_spr_bottom, 3},
    {"acc", akane_spr_acc, 3},
};

static const PortraitArea akane_portrait[] = {
    {"skin", 2, 8},
    {"hair", 32, 40},
    {"top", 14, 20},
    {"bottom", 25, 30},
    {"acc", 13, 9},
};

//=============================================================================
// AKIKO
// $pals: skin:[2,3,4,5], bottom:[9,10,11,12], eyes2:[13,14,15], eyes:[17,18], hair:[19,20,21,22], top:[23,24,25,26], top2:[29,30,31,32], acc:[27,28], unko:[37,38,39,40]
// $portraits: skin:[2,4], bottom:[9,12], eyes2:[13,15], eyes:[17,18], hair:[19,22], top:[23,26], top2:[29,32], acc:[27,28], unko:[33,35]
//=============================================================================
static const int8_t akiko_spr_skin[] = {2,3,4,5};
static const int8_t akiko_spr_bottom[] = {9,10,11,12};
static const int8_t akiko_spr_eyes2[] = {13,14,15};
static const int8_t akiko_spr_eyes[] = {17,18};
static const int8_t akiko_spr_hair[] = {19,20,21,22};
static const int8_t akiko_spr_top[] = {23,24,25,26};
static const int8_t akiko_spr_top2[] = {29,30,31,32};
static const int8_t akiko_spr_acc[] = {27,28};
static const int8_t akiko_spr_jam[] = {37,40};  // 0-based indices 36,39 (code does -1)

static const SpriteArea akiko_sprite[] = {
    {"skin", akiko_spr_skin, 4},
    {"bottom", akiko_spr_bottom, 4},
    {"eyes2", akiko_spr_eyes2, 3},
    {"eyes", akiko_spr_eyes, 2},
    {"hair", akiko_spr_hair, 4},
    {"top", akiko_spr_top, 4},
    {"top2", akiko_spr_top2, 4},
    {"acc", akiko_spr_acc, 2},
    {"jam", akiko_spr_jam, 2},
};

static const PortraitArea akiko_portrait[] = {
    {"skin", 2, 4},
    {"bottom", 9, 12},
    {"eyes2", 13, 15},
    {"eyes", 17, 18},
    {"hair", 19, 22},
    {"top", 23, 26},
    {"top2", 29, 32},
    {"acc", 27, 28},
    {"jam", 32, 34},  // Only applied if sprite indices 36,39 exist in .pal
};

//=============================================================================
// AYU
// $pals: skin:[1,2,3], eyes:[4,5], eyes2:[15,13,12], hair:[37,11,10,9], top:[8,7,6], wings:[19,17,18]
// Portrait eye slots verified in-game: eyes:[25,29] (dark->bright)
// Note: sprite indices 4/5 overlap accessory (headband) intentionally in source palette data
//=============================================================================
static const int8_t ayu_spr_skin[] = {1,2,3};
static const int8_t ayu_spr_eyes[] = {5,4};
static const int8_t ayu_spr_hair[] = {37,11,10,9};
static const int8_t ayu_spr_top[] = {8,7,6};
static const int8_t ayu_spr_eyes2[] = {15,13,12};
static const int8_t ayu_spr_wings[] = {19,17,18};

static const SpriteArea ayu_sprite[] = {
    {"skin", ayu_spr_skin, 3},
    {"eyes", ayu_spr_eyes, 2},
    {"hair", ayu_spr_hair, 4},
    {"top", ayu_spr_top, 3},
    {"eyes2", ayu_spr_eyes2, 3},
    {"wings", ayu_spr_wings, 3},
};

static const PortraitArea ayu_portrait[] = {
    {"skin", 1, 7},
    {"hair", 16, 8},
    {"top", 23, 18},
    {"eyes", 25, 29},
    {"eyes2", 34, 39},
    {"wings", 34, 32},
};

//=============================================================================
// DOPPEL
// $pals: skin:[1,2,3,4], eyes2:[14,16,22,23], hair:[5,6,7,25], top:[18,19,11], bottom:[8,9,10], acc:[17,21,12,13,24]
// $portraits: skin:[1,5], hair:[25,30], top:[17,21], bottom:[6,10], acc:[33,36]
// Note: eyes2 not in portraits
//=============================================================================
static const int8_t doppel_spr_skin[] = {1,2,3,4};
static const int8_t doppel_spr_hair[] = {5,6,7,25};
static const int8_t doppel_spr_top[] = {18,19,11};
static const int8_t doppel_spr_bottom[] = {8,9,10};
static const int8_t doppel_spr_acc[] = {17,21,12,13,24};

static const SpriteArea doppel_sprite[] = {
    {"skin", doppel_spr_skin, 4},
    {"hair", doppel_spr_hair, 4},
    {"top", doppel_spr_top, 3},
    {"bottom", doppel_spr_bottom, 3},
    {"acc", doppel_spr_acc, 5},
};

static const PortraitArea doppel_portrait[] = {
    {"skin", 1, 5},
    {"hair", 25, 30},
    {"top", 17, 21},
    {"bottom", 6, 10},
    {"acc", 33, 36},
};

//=============================================================================
// IKUMI
// $pals: skin:[2,3,4,5,6], eyes2:[38,39,40], hair:[10,11,12,13], top:[21,22,23], acc:[17,18,19,20]
// $portraits: skin:[2,6], hair:[7,12], acc:[17,21], top:[22,26], eyes2:[37,40]
//=============================================================================
static const int8_t ikumi_spr_skin[] = {2,3,4,5,6};
static const int8_t ikumi_spr_hair[] = {10,11,12,13};
static const int8_t ikumi_spr_acc[] = {17,18,19,20};
static const int8_t ikumi_spr_top[] = {21,22,23};
static const int8_t ikumi_spr_eyes2[] = {38,39,40};

static const SpriteArea ikumi_sprite[] = {
    {"skin", ikumi_spr_skin, 5},
    {"hair", ikumi_spr_hair, 4},
    {"acc", ikumi_spr_acc, 4},
    {"top", ikumi_spr_top, 3},
    {"eyes2", ikumi_spr_eyes2, 3},
};

static const PortraitArea ikumi_portrait[] = {
    {"skin", 2, 6},
    {"hair", 7, 12},
    {"acc", 17, 21},
    {"top", 22, 26},
    {"eyes2", 37, 40},
};

//=============================================================================
// KANNA
// $pals: skin:[2,3,4,5], hair:[12,11,9,10], top:[22,23,24,25], bottom:[19,20,21], wings:[31,30,29]
// $portraits: skin:[2,7], hair:[8,13], top:[25,30], bottom:[19,24], wings:[36,30]
//=============================================================================
static const int8_t kanna_spr_skin[] = {2,3,4,5};
static const int8_t kanna_spr_hair[] = {12,11,9,10};
static const int8_t kanna_spr_top[] = {22,23,24,25};
static const int8_t kanna_spr_bottom[] = {19,20,21};
static const int8_t kanna_spr_wings[] = {31,30,29};

static const SpriteArea kanna_sprite[] = {
    {"skin", kanna_spr_skin, 4},
    {"hair", kanna_spr_hair, 4},
    {"top", kanna_spr_top, 4},
    {"bottom", kanna_spr_bottom, 3},
    {"wings", kanna_spr_wings, 3},
};

static const PortraitArea kanna_portrait[] = {
    {"skin", 2, 7},
    {"hair", 8, 13},
    {"top", 25, 30},
    {"bottom", 19, 24},
    {"wings", 36, 30},
};

//=============================================================================
// KANO
// $pals: skin:[2,3,4,5], hair:[6,7,8,9], top:[10,11,12], bottom:[17,18,19,20], eyes:[23,24], acc:[25,26,27,28]
// $portraits: skin:[2,7], top:[8,11], bottom:[14,19], eyes:[20,24], acc:[25,29], hair:[34,40]
//=============================================================================
static const int8_t kano_spr_skin[] = {2,3,4,5};
static const int8_t kano_spr_hair[] = {6,7,8,9};
static const int8_t kano_spr_top[] = {10,11,12};
static const int8_t kano_spr_bottom[] = {17,18,19,20};
static const int8_t kano_spr_eyes[] = {23,24};
static const int8_t kano_spr_acc[] = {25,26,27,28};

static const SpriteArea kano_sprite[] = {
    {"skin", kano_spr_skin, 4},
    {"hair", kano_spr_hair, 4},
    {"top", kano_spr_top, 3},
    {"bottom", kano_spr_bottom, 4},
    {"eyes", kano_spr_eyes, 2},
    {"acc2", kano_spr_acc, 4},
};

static const PortraitArea kano_portrait[] = {
    {"skin", 2, 7},
    {"top", 8, 12},
    {"bottom", 14, 19},
    {"eyes", 20, 24},
    {"acc2", 25, 29},
    {"hair", 34, 40},
};

//=============================================================================
// KAORI
// $pals: skin:[1,2,3,4], hair:[19,20,21,22], top:[13,8,15], bottom:[9,10,11], acc:[24,25,26], eyes:[17,18], eyes2(socks):[6,7,5,12]
// $portraits: skin:[1,5], hair:[24,29], acc:[33,36], bottom:[17,20], eyes:[37,40], top:[6,10], eyes2:[13,15]
// Note: eyes2/socks mapping removed - sprite indices 6,7,5,12 are socks which don't exist on portrait
//=============================================================================
static const int8_t kaori_spr_skin[] = {1,2,3,4};
static const int8_t kaori_spr_hair[] = {19,20,21,22};
static const int8_t kaori_spr_top[] = {13,8,15};
static const int8_t kaori_spr_bottom[] = {9,10,11};
static const int8_t kaori_spr_acc[] = {24,25,26};
static const int8_t kaori_spr_eyes[] = {17,18};

static const SpriteArea kaori_sprite[] = {
    {"skin", kaori_spr_skin, 4},
    {"hair", kaori_spr_hair, 4},
    {"top", kaori_spr_top, 3},
    {"bottom", kaori_spr_bottom, 3},
    {"acc", kaori_spr_acc, 3},
    {"eyes", kaori_spr_eyes, 2},
};

static const PortraitArea kaori_portrait[] = {
    {"skin", 1, 5},
    {"hair", 24, 29},
    {"acc", 33, 36},
    {"bottom", 17, 20},
    {"eyes", 37, 40},
    {"top", 6, 10},
};

//=============================================================================
// MAKOTO
// $pals: skin:[2,3,4,5], hair:[12,11,10,9], top:[8,7,6] (yellowish sweater), top2:[16,15,14,13] (blue collar), bottom:[28,27,26,25,24], acc:[23,22,21]
// $portraits: skin:[2,7], top:[12,8], hair:[22,17], top2:[28,23], acc:[31,29], bottom:[36,32]
// NOTE: top and top2 are separate clothing pieces - top is cream/yellow sweater, top2 is blue by defaullt
//=============================================================================
static const int8_t makoto_spr_skin[] = {2,3,4,5};
static const int8_t makoto_spr_hair[] = {12,11,10,9};
static const int8_t makoto_spr_top[] = {8,7,6};  // Yellowish sweater only (was incorrectly mixing blue)
static const int8_t makoto_spr_top2[] = {16,15,14,13};// blue by default
static const int8_t makoto_spr_bottom[] = {28,27,26,25,24};
static const int8_t makoto_spr_acc[] = {23,22,21};

static const SpriteArea makoto_sprite[] = {
    {"skin", makoto_spr_skin, 4},
    {"hair", makoto_spr_hair, 4},
    {"top", makoto_spr_top, 3},  // 3 yellowish colors
    {"top2", makoto_spr_top2, 4},
    {"bottom", makoto_spr_bottom, 5},
    {"acc", makoto_spr_acc, 3},
};

static const PortraitArea makoto_portrait[] = {
    {"skin", 2, 7},
    {"top", 12, 8},
    {"hair", 22, 17},
    {"top2", 28, 23},
    {"acc", 31, 29},
    {"bottom", 36, 32},
};

//=============================================================================
// MAI
// $pals: skin:[1,2,3,4], hair:[5,6,7,14], bottom:[11,12,13], mai:[23,24,25], acc:[9,10], ears:[17,18], eyes:[21,22]
// $portraits: skin:[2,7], hair:[24,17], bottom:[12,8], mai:[13,16], acc:[40,36], eyes:[35,32], ears:[13,16]
//=============================================================================
static const int8_t mai_spr_skin[] = {1,2,3,4};
static const int8_t mai_spr_hair[] = {5,6,7,14};
static const int8_t mai_spr_bottom[] = {11,12,13};
static const int8_t mai_spr_mai[] = {23,24,25};
static const int8_t mai_spr_acc[] = {9,10};
static const int8_t mai_spr_ears[] = {17,18};
static const int8_t mai_spr_eyes[] = {21,22};

static const SpriteArea mai_sprite[] = {
    {"skin", mai_spr_skin, 4},
    {"hair", mai_spr_hair, 4},
    {"bottom", mai_spr_bottom, 3},
    {"mai", mai_spr_mai, 3},
    {"acc", mai_spr_acc, 2},
    {"ears", mai_spr_ears, 2},
    {"eyes", mai_spr_eyes, 2},
};

static const PortraitArea mai_portrait[] = {
    {"skin", 2, 7},
    {"hair", 24, 17},
    {"bottom", 12, 8},
    {"mai", 13, 16},   // Mini-Mai's yellow area on portrait (0-based: 12-15)
    {"acc", 40, 36},
    {"eyes", 35, 32},
    {"ears", 29, 31},  // Bunny Ears on portrait (0-based: 28-30)
};

//=============================================================================
// MAYU
// $pals: skin:[5,4,3,2], eyes2:[13,12,11,10], hair:[9,8,7], top:[19,18,17], top2:[28,27,26], bottom:[15,14,25]
// $portraits: skin:[7,2], hair:[14,8], top:[21,17], top2:[26,31], eyes2:[25,23], bottom:[38,33]
//=============================================================================
static const int8_t mayu_spr_skin[] = {5,4,3,2};
static const int8_t mayu_spr_hair[] = {9,8,7};
static const int8_t mayu_spr_top[] = {19,18,17};
static const int8_t mayu_spr_top2[] = {28,27,26};
static const int8_t mayu_spr_eyes2[] = {13,12,11,10};
static const int8_t mayu_spr_bottom[] = {15,14,25};

static const SpriteArea mayu_sprite[] = {
    {"skin", mayu_spr_skin, 4},
    {"hair", mayu_spr_hair, 3},
    {"top", mayu_spr_top, 3},
    {"top2", mayu_spr_top2, 3},
    {"eyes2", mayu_spr_eyes2, 4},
    {"bottom", mayu_spr_bottom, 3},
};

static const PortraitArea mayu_portrait[] = {
    {"skin", 7, 2},
    {"hair", 14, 8},
    {"top", 21, 17},
    {"top2", 26, 31},
    {"eyes2", 25, 23},
    {"bottom", 38, 33},
};

//=============================================================================
// MINAGI
// $pals: skin:[2,3,4,5], eyes2:[9,7,8], hair:[13,14,15,16], top:[10,11,12], bottom:[17,18,19,20], acc:[21,22,23,24], hair2:[25,26,27,32]
// $portraits: skin:[2,7], hair:[17,22], top:[12,16], bottom:[23,28], hair2:[34,38], acc:[29,33], eyes2:[11,9]
//=============================================================================
static const int8_t minagi_spr_skin[] = {2,3,4,5};
static const int8_t minagi_spr_hair[] = {13,14,15,16};
static const int8_t minagi_spr_top[] = {10,11,12};
static const int8_t minagi_spr_bottom[] = {17,18,19,20};
static const int8_t minagi_spr_hair2[] = {25,26,27,32};
static const int8_t minagi_spr_acc[] = {21,22,23,24};
static const int8_t minagi_spr_eyes2[] = {9,7,8};

static const SpriteArea minagi_sprite[] = {
    {"skin", minagi_spr_skin, 4},
    {"hair", minagi_spr_hair, 4},
    {"top", minagi_spr_top, 3},
    {"bottom", minagi_spr_bottom, 4},
    {"hair2", minagi_spr_hair2, 4},
    {"acc", minagi_spr_acc, 4},
    {"eyes2", minagi_spr_eyes2, 3},
};

static const PortraitArea minagi_portrait[] = {
    {"skin", 2, 7},
    {"hair", 17, 22},
    {"top", 12, 16},
    {"bottom", 23, 28},
    {"hair2", 34, 38},
    {"acc", 29, 33},
    {"eyes2", 11, 9},
};

//=============================================================================
// MIO
// $pals: skin:[1,2,3,4], eyes:[22,23], hair:[5,6,7], top:[21,20,19,18], acc:[14,15], acc2:[31,24,25]
// $portraits: skin:[2,7], hair:[8,13], top:[21,16], eyes:[22,25], book:[34,36], acc:[37,40], acc2:[26,29]
//=============================================================================
static const int8_t mio_spr_skin[] = {1,2,3,4};
static const int8_t mio_spr_hair[] = {5,6,7};
static const int8_t mio_spr_top[] = {21,20,19,18};
static const int8_t mio_spr_eyes[] = {22,23};
static const int8_t mio_spr_acc[] = {14,15};  // This is 'book' in sprite pals
static const int8_t mio_spr_acc2[] = {31,24,25};

static const SpriteArea mio_sprite[] = {
    {"skin", mio_spr_skin, 4},
    {"hair", mio_spr_hair, 3},
    {"top", mio_spr_top, 4},
    {"eyes", mio_spr_eyes, 2},
    {"acc", mio_spr_acc, 2},
    {"acc2", mio_spr_acc2, 3},
};

static const PortraitArea mio_portrait[] = {
    {"skin", 2, 7},
    {"hair", 8, 13},
    {"top", 21, 16},
    {"eyes", 22, 25},
    {"acc", 26, 29},   // notebook (sprite acc = 14,15)
    {"acc2", 37, 40},  // head accessory (sprite acc2 = 31,24,25)
};

//=============================================================================
// MISAKI
// $pals: skin:[2,3,4,5], eyes2:[12,15], hair:[6,7,8,11], top:[16,18,19,17], bottom:[9,10,14]
// $portraits: skin:[2,8], eyes2:[10,14], top:[15,20], hair:[22,29], bottom:[33,39]
//=============================================================================
static const int8_t misaki_spr_skin[] = {2,3,4,5};
static const int8_t misaki_spr_hair[] = {6,7,8};  // Exclude 11 (saturated navy outline) - use 8 as dark endpoint
static const int8_t misaki_spr_top[] = {16,18,19,17};
static const int8_t misaki_spr_bottom[] = {9,10,14};
static const int8_t misaki_spr_eyes2[] = {12,15};

static const SpriteArea misaki_sprite[] = {
    {"skin", misaki_spr_skin, 4},
    {"hair", misaki_spr_hair, 3},
    {"top", misaki_spr_top, 4},
    {"bottom", misaki_spr_bottom, 3},
    {"eyes2", misaki_spr_eyes2, 2},
};

static const PortraitArea misaki_portrait[] = {
    {"skin", 2, 8},
    {"eyes2", 10, 14},
    {"top", 15, 20},
    {"hair", 22, 30},  // Fixed: was 22,29 - should be 22,30 per PS1 script
    {"bottom", 33, 39},
};

//=============================================================================
// MISHIO
// $pals: skin:[2,3,4,5], eyes:[31,32], eyes2:[40,39,38], hair:[17,18,19,20], top:[6,8,9,10,11], bottom:[24,25,26], boots:[14,15,16], spear_outline:[7], spear:[9,10], acc:[21,22,23]
// $portraits: skin:[2,6], hair:[17,22], eyes2:[9,12], spear_outline:[30,30], spear:[14,15], bottom:[23,27], top:[32,32], acc:[33,36], eyes:[37,40]
//=============================================================================
static const int8_t mishio_spr_skin[] = {2,3,4,5};
static const int8_t mishio_spr_hair[] = {17,18,19,20};
static const int8_t mishio_spr_top[] = {6,8,9,10,11};
static const int8_t mishio_spr_bottom[] = {24,25,26};
static const int8_t mishio_spr_boots[] = {14,15,16};
static const int8_t mishio_spr_spear_outline[] = {7};
static const int8_t mishio_spr_spear[] = {9,10};
static const int8_t mishio_spr_acc[] = {21,22,23};
static const int8_t mishio_spr_eyes[] = {31,32};
static const int8_t mishio_spr_eyes2[] = {40,39,38};

static const SpriteArea mishio_sprite[] = {
    {"skin", mishio_spr_skin, 4},
    {"hair", mishio_spr_hair, 4},
    {"top", mishio_spr_top, 5},
    {"bottom", mishio_spr_bottom, 3},
    {"boots", mishio_spr_boots, 3},
    {"spear_outline", mishio_spr_spear_outline, 1},
    {"spear", mishio_spr_spear, 2},
    {"acc", mishio_spr_acc, 3},
    {"eyes", mishio_spr_eyes, 2},
    {"eyes2", mishio_spr_eyes2, 3},
};

static const PortraitArea mishio_portrait[] = {
    {"skin", 2, 6},
    {"hair", 17, 22},
    {"eyes2", 9, 12},
    {"spear_outline", 30, 30},
    {"spear", 14, 15},
    {"bottom", 23, 27},
    {"top", 32, 32},
    {"acc", 33, 36},
    {"eyes", 37, 40},
};

//=============================================================================
// MISUZU
// $pals: skin:[2,3,4,5], hair:[6,7,8,9], top:[25,26,27,28], bottom:[10,11,12,13], eyes:[14,15,16], acc:[17,18,19,20], acc2:[21,22,23,24], eyes2:[38,39,40]
// $portraits: skin:[2,6], hair:[7,12], bottom:[13,17], eyes:[18,21], acc:[22,25], acc2:[26,29], top:[30,34], eyes2:[36,38]
//=============================================================================
static const int8_t misuzu_spr_skin[] = {2,3,4,5};
static const int8_t misuzu_spr_hair[] = {6,7,8,9};
static const int8_t misuzu_spr_top[] = {25,26,27,28};
static const int8_t misuzu_spr_bottom[] = {10,11,12,13};
static const int8_t misuzu_spr_eyes[] = {14,15,16};
static const int8_t misuzu_spr_acc[] = {17,18,19,20};
static const int8_t misuzu_spr_acc2[] = {21,22,23,24};
static const int8_t misuzu_spr_eyes2[] = {38,39,40};

static const SpriteArea misuzu_sprite[] = {
    {"skin", misuzu_spr_skin, 4},
    {"hair", misuzu_spr_hair, 4},
    {"top", misuzu_spr_top, 4},
    {"bottom", misuzu_spr_bottom, 4},
    {"eyes", misuzu_spr_eyes, 3},
    {"acc", misuzu_spr_acc, 4},
    {"acc2", misuzu_spr_acc2, 4},
    {"eyes2", misuzu_spr_eyes2, 3},
};

static const PortraitArea misuzu_portrait[] = {
    {"skin", 2, 6},
    {"hair", 7, 12},
    {"bottom", 13, 17},
    {"eyes", 18, 21},
    {"acc", 22, 25},
    {"acc2", 26, 29},
    {"top", 30, 34},
    {"eyes2", 36, 38},
};

//=============================================================================
// MIZUKA (Nagamori)
// $pals: skin:[2,3,4,5], eyes2:[12,13,14,15], hair:[20,23,21,24,22], top:[6,7,8], bottom:[9,10,11], acc:[17,18,19], acc2:[26,27]
// $portraits: skin:[2,7], hair:[27,31], top:[8,12], bottom:[13,16], acc:[22,26], acc2:[33,36]
// Note: eyes2 contains the hair/eye outline colors. Portrait mapping intentionally omits eyes2
// to preserve the original outline colors from the base palette.
//=============================================================================
static const int8_t mizuka_spr_skin[] = {2,3,4,5};
static const int8_t mizuka_spr_hair[] = {20,23,21,24,22};
static const int8_t mizuka_spr_top[] = {6,7,8};
static const int8_t mizuka_spr_bottom[] = {9,10,11};
static const int8_t mizuka_spr_acc[] = {17,18,19};
static const int8_t mizuka_spr_acc2[] = {26,27};

static const SpriteArea mizuka_sprite[] = {
    {"skin", mizuka_spr_skin, 4},
    {"hair", mizuka_spr_hair, 5},
    {"top", mizuka_spr_top, 3},
    {"bottom", mizuka_spr_bottom, 3},
    {"acc", mizuka_spr_acc, 3},
    {"acc2", mizuka_spr_acc2, 2},
};

static const PortraitArea mizuka_portrait[] = {
    {"skin", 2, 7},
    {"hair", 27, 31},
    {"top", 8, 12},
    {"bottom", 13, 16},
    {"acc", 22, 26},
    {"acc2", 33, 36},
};

//=============================================================================
// NAYUKI (awake - NayukiB)
// $pals: skin:[2,3,4,5], eyes:[17,18], eyes2:[13,14,15], socks:[9,8,7,6], hair:[19,20,21,22], top:[29,30,31,32], bottom:[10,11,12], acc:[23,24,25], bag:[33,34,35,36], unko:[26,27,28]
// $portraits: skin:[2,5], eyes:[17,18], eyes2:[13,15], socks:[9,6], hair:[19,22], top:[29,32], bottom:[10,12], acc:[23,25], bag:[33,36], unko:[26,28]
//=============================================================================
static const int8_t nayuki_spr_skin[] = {2,3,4,5};
static const int8_t nayuki_spr_eyes[] = {17,18};
static const int8_t nayuki_spr_eyes2[] = {13,14,15};
static const int8_t nayuki_spr_socks[] = {9,8,7,6};
static const int8_t nayuki_spr_hair[] = {19,20,21,22};
static const int8_t nayuki_spr_top[] = {29,30,31,32};
static const int8_t nayuki_spr_bottom[] = {10,11,12};
static const int8_t nayuki_spr_acc[] = {23,24,25};
static const int8_t nayuki_spr_bag[] = {33,34,35,36};
static const int8_t nayuki_spr_unko[] = {26,27,28};

static const SpriteArea nayuki_sprite[] = {
    {"skin", nayuki_spr_skin, 4},
    {"eyes", nayuki_spr_eyes, 2},
    {"eyes2", nayuki_spr_eyes2, 3},
    {"socks", nayuki_spr_socks, 4},
    {"hair", nayuki_spr_hair, 4},
    {"top", nayuki_spr_top, 4},
    {"bottom", nayuki_spr_bottom, 3},
    {"acc", nayuki_spr_acc, 3},
    {"bag", nayuki_spr_bag, 4},
    {"unko", nayuki_spr_unko, 3},
};

static const PortraitArea nayuki_portrait[] = {
    {"skin", 2, 5},
    {"eyes", 17, 18},
    {"eyes2", 13, 15},
    {"socks", 9, 6},
    {"hair", 19, 22},
    {"top", 29, 32},
    {"bottom", 10, 12},
    {"acc", 23, 25},
    {"bag", 33, 36},
    {"unko", 26, 28},
};

//=============================================================================
// NEYUKI (sleepy - Nayuki)
// $pals: skin:[3,4,5,6,7,8], hair:[9,10,11,12], top:[25,26,27,28], bottom:[13,14,15,16], kero:[22,23,24]
// $portraits: skin:[2,8], hair:[8,13], top:[17,22], eyes2:[29,31], kero:[33,37]
// Note: bottom maps to eyes2 in portrait. Portrait hair ends at 13 (14 is black)
//=============================================================================
static const int8_t neyuki_spr_skin[] = {3,4,5,6,7,8};
static const int8_t neyuki_spr_hair[] = {9,10,11,12};
static const int8_t neyuki_spr_top[] = {25,26,27,28};
static const int8_t neyuki_spr_eyes2[] = {13,14,15,16}; // bottom in sprite
static const int8_t neyuki_spr_kero[] = {22,23,24};

static const SpriteArea neyuki_sprite[] = {
    {"skin", neyuki_spr_skin, 6},
    {"hair", neyuki_spr_hair, 4},
    {"top", neyuki_spr_top, 4},
    {"eyes2", neyuki_spr_eyes2, 4},
    {"kero", neyuki_spr_kero, 3},
};

static const PortraitArea neyuki_portrait[] = {
    {"skin", 2, 8},
    {"hair", 8, 13},
    {"top", 17, 22},
    {"eyes2", 29, 31},
    {"kero", 33, 37},
};

//=============================================================================
// RUMI (Nanase)
// $pals: skin:[1,2,3,4], eyes2:[14,16,22,23], hair:[5,6,7], top:[18,19,11], bottom:[8,9,10], acc:[17,21,12,13]
// $portraits: skin:[2,7], eyes:[20,24], hair:[8,13], top:[30,34], bottom:[14,19], acc:[25,29]
// Note: eyes2 in sprite maps to eyes in portrait
// Note: Removed index 25 from hair (was dark blue, not part of hair gradient)
//=============================================================================
static const int8_t rumi_spr_skin[] = {1,2,3,4};
static const int8_t rumi_spr_hair[] = {5,6,7};  // Removed 25 (was wrong - dark blue not hair)
static const int8_t rumi_spr_top[] = {18,19,11};
static const int8_t rumi_spr_bottom[] = {8,9,10};
static const int8_t rumi_spr_acc[] = {17,21,12,13};
static const int8_t rumi_spr_eyes[] = {14,16,22,23}; // eyes2 in sprite

static const SpriteArea rumi_sprite[] = {
    {"skin", rumi_spr_skin, 4},
    {"hair", rumi_spr_hair, 3},  // Changed from 4 to 3
    {"top", rumi_spr_top, 3},
    {"bottom", rumi_spr_bottom, 3},
    {"acc", rumi_spr_acc, 4},
    {"eyes", rumi_spr_eyes, 4},
};

static const PortraitArea rumi_portrait[] = {
    {"skin", 2, 7},
    {"eyes", 20, 24},
    {"hair", 8, 13},
    {"top", 30, 34},
    {"bottom", 14, 19},
    {"acc", 25, 29},
};

//=============================================================================
// SAYURI
// $pals: skin:[1,2,3,4], eyes:[15,14,13], eyes2:[30,31,32], hair:[17,18,19,20], top:[24,25,26,27,28,29], bottom:[9,10,11], acc:[5,6,7,8], acc2:[21,22,23], gun:[33,34,35]
// $portraits: skin:[2,6], hair:[33,37], bottom:[7,11], acc:[17,20], acc2:[12,14], eyes2:[21,25], eyes:[40,38], gun:[15,16]
//=============================================================================
static const int8_t sayuri_spr_skin[] = {1,2,3,4};
static const int8_t sayuri_spr_hair[] = {17,18,19,20};
static const int8_t sayuri_spr_bottom[] = {9,10,11};
static const int8_t sayuri_spr_acc[] = {5,6,7,8};
static const int8_t sayuri_spr_acc2[] = {21,22,23};
static const int8_t sayuri_spr_eyes[] = {15,14,13};
static const int8_t sayuri_spr_eyes2[] = {30,31,32};
static const int8_t sayuri_spr_gun[] = {33,34,35};

static const SpriteArea sayuri_sprite[] = {
    {"skin", sayuri_spr_skin, 4},
    {"hair", sayuri_spr_hair, 4},
    {"bottom", sayuri_spr_bottom, 3},
    {"acc", sayuri_spr_acc, 4},
    {"acc2", sayuri_spr_acc2, 3},
    {"eyes", sayuri_spr_eyes, 3},
    {"eyes2", sayuri_spr_eyes2, 3},
    {"gun", sayuri_spr_gun, 3},
};

static const PortraitArea sayuri_portrait[] = {
    {"skin", 2, 6},
    {"hair", 33, 37},
    {"bottom", 7, 11},
    {"acc", 17, 20},
    {"acc2", 12, 14},
    {"eyes2", 21, 25},
    {"eyes", 40, 38},
    {"gun", 15, 16},
};

//=============================================================================
// SHIORI
// $pals: skin:[8,7,6,5], eyes:[30,29], hair:[4,3,2,1], top:[12,11,10,9], bottom:[16,15,14,13], acc:[20,19,18,17], scarf1:[24,22,25], scarf2:[28,27,26]
// $portraits: skin:[7,2], hair:[12,8], top:[16,13], bottom:[32,29], acc:[20,17], eyes:[35,33], scarf1:[24,21], scarf2:[28,25]
//=============================================================================
static const int8_t shiori_spr_skin[] = {8,7,6,5};
static const int8_t shiori_spr_hair[] = {4,3,2,1};
static const int8_t shiori_spr_top[] = {12,11,10,9};
static const int8_t shiori_spr_bottom[] = {16,15,14,13};
static const int8_t shiori_spr_acc[] = {20,19,18,17};
static const int8_t shiori_spr_eyes[] = {30,29};
static const int8_t shiori_spr_scarf1[] = {24,22,25};
static const int8_t shiori_spr_scarf2[] = {28,27,26};

static const SpriteArea shiori_sprite[] = {
    {"skin", shiori_spr_skin, 4},
    {"hair", shiori_spr_hair, 4},
    {"top", shiori_spr_top, 4},
    {"bottom", shiori_spr_bottom, 4},
    {"acc", shiori_spr_acc, 4},
    {"eyes", shiori_spr_eyes, 2},
    {"scarf1", shiori_spr_scarf1, 3},
    {"scarf2", shiori_spr_scarf2, 3},
};

static const PortraitArea shiori_portrait[] = {
    {"skin", 7, 2},
    {"hair", 12, 8},
    {"top", 16, 13},
    {"bottom", 32, 29},
    {"acc", 20, 17},
    {"eyes", 35, 33},
    {"scarf1", 24, 21},
    {"scarf2", 28, 25},
};

//=============================================================================
// UNKNOWN (MizukaB)
// $pals: skin:[2,3,4,5], eyes2:[22,23], hair:[6,7,17,19], top:[9,20,10,18,15], acc2:[12,13], eyes:[11,14]
// $portraits: skin:[2,7], hair:[8,14], top:[23,18], acc2:[40,37], eyes:[29,32], eyes2:[27,24]
//=============================================================================
static const int8_t unknown_spr_skin[] = {2,3,4,5};
static const int8_t unknown_spr_hair[] = {6,7,17,19};
static const int8_t unknown_spr_top[] = {9,20,10,18,15};
static const int8_t unknown_spr_acc2[] = {12,13};
static const int8_t unknown_spr_eyes[] = {11,14};
static const int8_t unknown_spr_eyes2[] = {22,23};

static const SpriteArea unknown_sprite[] = {
    {"skin", unknown_spr_skin, 4},
    {"hair", unknown_spr_hair, 4},
    {"top", unknown_spr_top, 5},
    {"acc2", unknown_spr_acc2, 2},
    {"eyes", unknown_spr_eyes, 2},
    {"eyes2", unknown_spr_eyes2, 2},
};

static const PortraitArea unknown_portrait[] = {
    {"skin", 2, 7},
    {"hair", 8, 14},
    {"top", 23, 18},
    {"acc2", 40, 37},
    {"eyes", 29, 32},
    {"eyes2", 27, 24},
};

//=============================================================================
// Master Character Mapping Table
//=============================================================================

#define CHAR_MAPPING(name, spr, port) \
    {name, spr, sizeof(spr)/sizeof(spr[0]), port, sizeof(port)/sizeof(port[0])}

static const CharacterMapping g_CharacterMappings[] = {
    CHAR_MAPPING("akane", akane_sprite, akane_portrait),
    CHAR_MAPPING("akiko", akiko_sprite, akiko_portrait),
    CHAR_MAPPING("ayu", ayu_sprite, ayu_portrait),
    CHAR_MAPPING("doppel", doppel_sprite, doppel_portrait),
    CHAR_MAPPING("exnanase", doppel_sprite, doppel_portrait),
    CHAR_MAPPING("ikumi", ikumi_sprite, ikumi_portrait),
    CHAR_MAPPING("kanna", kanna_sprite, kanna_portrait),
    CHAR_MAPPING("kano", kano_sprite, kano_portrait),
    CHAR_MAPPING("kaori", kaori_sprite, kaori_portrait),
    CHAR_MAPPING("makoto", makoto_sprite, makoto_portrait),
    CHAR_MAPPING("mai", mai_sprite, mai_portrait),
    CHAR_MAPPING("mayu", mayu_sprite, mayu_portrait),
    CHAR_MAPPING("minagi", minagi_sprite, minagi_portrait),
    CHAR_MAPPING("mio", mio_sprite, mio_portrait),
    CHAR_MAPPING("misaki", misaki_sprite, misaki_portrait),
    CHAR_MAPPING("mishio", mishio_sprite, mishio_portrait),
    CHAR_MAPPING("misuzu", misuzu_sprite, misuzu_portrait),
    CHAR_MAPPING("mizuka", mizuka_sprite, mizuka_portrait),
    CHAR_MAPPING("nagamori", mizuka_sprite, mizuka_portrait),
    CHAR_MAPPING("nayuki", neyuki_sprite, neyuki_portrait),    // Sleepy Nayuki
    CHAR_MAPPING("nayukib", nayuki_sprite, nayuki_portrait),   // Awake Nayuki
    CHAR_MAPPING("neyuki", neyuki_sprite, neyuki_portrait),
    CHAR_MAPPING("rumi", rumi_sprite, rumi_portrait),
    CHAR_MAPPING("nanase", rumi_sprite, rumi_portrait),
    CHAR_MAPPING("sayuri", sayuri_sprite, sayuri_portrait),
    CHAR_MAPPING("shiori", shiori_sprite, shiori_portrait),
    CHAR_MAPPING("unknown", unknown_sprite, unknown_portrait),
    CHAR_MAPPING("mizukab", unknown_sprite, unknown_portrait),
};

static const int g_NumMappings = sizeof(g_CharacterMappings) / sizeof(g_CharacterMappings[0]);

//=============================================================================
// Helper Functions
//=============================================================================

inline const CharacterMapping* FindCharacterMapping(const char* charName)
{
    char nameLower[64];
    int i = 0;
    for (; charName[i] && i < 63; i++) {
        char c = charName[i];
        nameLower[i] = (c >= 'A' && c <= 'Z') ? (c + 32) : c;
    }
    nameLower[i] = '\0';
    
    for (int j = 0; j < g_NumMappings; j++) {
        if (strcmp(nameLower, g_CharacterMappings[j].charName) == 0) {
            return &g_CharacterMappings[j];
        }
    }
    return nullptr;
}

inline const SpriteArea* FindSpriteArea(const CharacterMapping* mapping, const char* areaName)
{
    for (int i = 0; i < mapping->spriteAreaCount; i++) {
        if (strcmp(mapping->spriteAreas[i].name, areaName) == 0) {
            return &mapping->spriteAreas[i];
        }
    }
    return nullptr;
}
