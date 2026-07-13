# Sprite-effect table (data\sprite\이펙트\*.spr) — actor overlay effects

Third hat-effect resource type (besides name-based `.str` and EffectMgr procedural).

`hatEffectID` (id-based hat effects) whose folder name matches -> loaded as an actor overlay sprite.

Path = `data\sprite\이펙트` + string below (+ `.act`). Strings live hardcoded in the exe, indexed

by effect id via an offset table (no plain xref).


## id-based hat effects that are sprite effects

| hatEffectID | HAT_EF | sprite (data\sprite\이펙트\...) |
|---:|:--|:--|
| 1130 | HAT_EF_BAKURETSU_HADOU | \bakuretsu_hadou\bakuretsu_hadou.spr |
| 1240 | HAT_EF_Digital_Space | \digital_space\digital_space.spr |
| 1377 | HAT_EF_C_Valkyrie_Wing | \valkyrie_wing\valkyrie_wing.spr |
| 2346 | HAT_EF_BLACK_THUNDER | \Black_Thunder\Black_Thunder.spr |
| 2394 | HAT_EF_SERPENT_SHADOW | \Serpent_Shadow\Serpent_Shadow.spr |
| 2424 | HAT_EF_AURA_OF_GHOST_S | \C_Aura_Of_Ghost_S\C_Aura_Of_Ghost_S.spr |
| 2428 | HAT_EF_Atque_Poenitentia | \Atque_Poenitentia\Atque_Poenitentia.spr |
| 2429 | HAT_EF_Perm_Frost_Oblivion | \Perm_Frost_Oblivion\Perm_Frost_Oblivion.spr |
| 2430 | HAT_EF_GUIDE_OF_DEAD_TEXT | \C_Guide_Of_Dead_Text\C_Guide_Of_Dead_Text.spr |
| 2431 | HAT_EF_MEDJED_TEXT | \C_Medjed_Text\C_Medjed_Text.spr |

## full exe sprite-effect string block (all entries)

| addr | string | hat match |
|:--|:--|:--|
| 0x01086b5a | \bakuretsu_hadou\bakuretsu_hadou.spr | HAT_EF_BAKURETSU_HADOU=1130 |
| 0x01086bb2 | \EF_POPCORN\EF_POPCORN.spr |  |
| 0x01086bfa | \EF_IMMUNE_PROPERTY\EF_IMMUNE_PROPERTY_NOTHING.spr |  |
| 0x01086c72 | \EF_IMMUNE_PROPERTY\EF_IMMUNE_PROPERTY_WATER.spr |  |
| 0x01086ce2 | \EF_IMMUNE_PROPERTY\EF_IMMUNE_PROPERTY_GROUND.spr |  |
| 0x01086d52 | \EF_IMMUNE_PROPERTY\EF_IMMUNE_PROPERTY_FIRE.spr |  |
| 0x01086dc2 | \EF_IMMUNE_PROPERTY\EF_IMMUNE_PROPERTY_WIND.spr |  |
| 0x01086e32 | \EF_IMMUNE_PROPERTY\EF_IMMUNE_PROPERTY_POISON.spr |  |
| 0x01086ea2 | \EF_IMMUNE_PROPERTY\EF_IMMUNE_PROPERTY_SAINT.spr |  |
| 0x01086f12 | \EF_IMMUNE_PROPERTY\EF_IMMUNE_PROPERTY_DARKNESS.spr |  |
| 0x01086f8a | \EF_IMMUNE_PROPERTY\EF_IMMUNE_PROPERTY_TELEKINESIS.spr |  |
| 0x0108700a | \EF_IMMUNE_PROPERTY\EF_IMMUNE_PROPERTY_UNDEAD.spr |  |
| 0x0108707a | \random_opt_spr\yellow.spr |  |
| 0x010870c2 | \subject_aura\subject_aura.spr | HAT_EF_SUBJECT_AURA_NAVY=2301 |
| 0x01087112 | \book_of_dimension\book_of_dimension.spr |  |
| 0x01087172 | \book_of_creatingstar\book_of_creatingstar.spr |  |
| 0x010871e2 | \curse_explosion\curse_explosion.spr |  |
| 0x0108723a | \soul_reaper\soul_reaper.spr |  |
| 0x01087282 | \soul_shadow\soul_shadow.spr |  |
| 0x010872ca | \soul_fairy\soul_fairy.spr |  |
| 0x01087312 | \soul_falcon\soul_falcon.spr |  |
| 0x0108735a | \soul_explosion\soul_explosion.spr |  |
| 0x010873b2 | \valkyrie_wing\valkyrie_wing.spr | HAT_EF_C_Valkyrie_Wing=1377 |
| 0x01087402 | \c_amdarais_effect.spr |  |
| 0x01087442 | \black_bubble\small\blackbubble_small.spr |  |
| 0x010874a2 | \black_bubble\midium\blackbubble_midium.spr |  |
| 0x0108750a | \black_bubble\large\blackbubble_large.spr |  |
| 0x0108756a | \Serpent_Shadow\Serpent_Shadow.spr | HAT_EF_SERPENT_SHADOW=2394 |
| 0x010875c2 | \violen_hit.spr |  |
| 0x010875f2 | \C_Aura_Of_Ghost_S\C_Aura_Of_Ghost_S.spr | HAT_EF_AURA_OF_GHOST_S=2424 |
| 0x01087652 | \C_Guide_Of_Dead_Text\C_Guide_Of_Dead_Text.spr | HAT_EF_GUIDE_OF_DEAD_TEXT=2430 |
| 0x010876c2 | \C_Medjed_Text\C_Medjed_Text.spr | HAT_EF_MEDJED_TEXT=2431 |
| 0x01087712 | \Atque_Poenitentia\Atque_Poenitentia.spr | HAT_EF_Atque_Poenitentia=2428 |
| 0x01087772 | \Perm_Frost_Oblivion\Perm_Frost_Oblivion.spr | HAT_EF_Perm_Frost_Oblivion=2429 |
| 0x0108e80e | \Black_Thunder\Black_Thunder.spr | HAT_EF_BLACK_THUNDER=2346 |
| 0x0108e85e | \Black_Thunder\Black_Thunder_Dark.spr | HAT_EF_BLACK_THUNDER=2346 |
| 0x0108e8b6 | \Barmund_Magic_Circle\Angel_Magic_Circle.spr |  |
| 0x0108e91e | \Barmund_Magic_Circle\Animal_Magic_Circle.spr |  |
| 0x0108e986 | \Barmund_Magic_Circle\Devil_Magic_Circle.spr |  |
| 0x0108e9ee | \Barmund_Magic_Circle\Dragon_Magic_Circle.spr |  |
| 0x0108ea56 | \Barmund_Magic_Circle\Fish_Magic_Circle.spr |  |
| 0x0108eabe | \Barmund_Magic_Circle\Human_Magic_Circle.spr |  |
| 0x0108eb26 | \Barmund_Magic_Circle\Insect_Magic_Circle.spr |  |
| 0x0108eb8e | \Barmund_Magic_Circle\Nothing_Magic_Circle.spr |  |
| 0x0108ebfe | \Barmund_Magic_Circle\Plant_Magic_Circle.spr |  |
| 0x0108ec66 | \Barmund_Magic_Circle\Undead_Magic_Circle.spr |  |
| 0x0108f09a | \miyabi_ningyo.spr |  |
| 0x0108f0d2 | \garden_keeper.spr |  |
| 0x0108f1d2 | \digital_space\digital_space.spr | HAT_EF_Digital_Space=1240 |