# Hat-effect table (HatEffectInfo.lub / HatEffectIDs.lub)

For each costume hat-effect: its ordinal (HatEFID), and WHAT IT POINTS TO — either a `.str`
resource (name-based, rendered directly) OR a numeric `hatEffectID` = a client effect id
passed to EffectMgr_SpawnEffect. The `effect-id -> .str` column is cross-referenced against
the dumped resolver table; `(procedural)` = id has no .str in either resolver (class-based effect).

| HatEFID | HAT_EF name | points to | resolver .str |
|----:|:--|:--|:--|
| 1 | HAT_EF_Blossom_Fluttering | resourceFileName | efst_blossom_fluttering\sakura.str |
| 2 | HAT_EF_MERMAID_LONGING | resourceFileName | efst_mermaid_loging\bubblebubble.str |
| 3 | HAT_EF_rl_banishing_buster | resourceFileName | rl_banishing_buster\vanishing1.str |
| 4 | HAT_EF_LJOSALFAR | resourceFileName | efst_ljosalfar\ljosalfar.str |
| 5 | HAT_EF_CLOCKING | effectID 120 (0x78) | (procedural / no .str) |
| 6 | HAT_EF_SNOW | effectID 162 (0xa2) | (procedural / no .str) |
| 7 | HAT_EF_MAKEBLUR | effectID 166 (0xa6) | (procedural / no .str) |
| 8 | HAT_EF_SLEEPATTACK | effectID 197 (0xc5) | (procedural / no .str) |
| 9 | HAT_EF_GUMGANG | effectID 203 (0xcb) | (procedural / no .str) |
| 10 | HAT_EF_TALK_FROSTJOKE | effectID 295 (0x127) | (procedural / no .str) |
| 11 | HAT_EF_DEMONSTRATION | effectID 302 (0x12e) | (procedural / no .str) |
| 12 | HAT_EF_Flutter_Butterfly | resourceFileName | efst_Flutter_Butterfly\Flutter_Butterfly.str |
| 13 | HAT_EF_Angel_Fluttering | resourceFileName | efst_Angel_Fluttering\Angel_Fluttering.str |
| 14 | HAT_EF_Blessing_Of_Angels | resourceFileName | efst_blessing_of_angels\tensi3.str |
| 15 | HAT_EF_Electric | effectID 254 (0xfe) | (procedural / no .str) |
| 16 | HAT_EF_Green_Floor | effectID 680 (0x2a8) | (procedural / no .str) |
| 17 | HAT_EF_Shrink | effectID 421 (0x1a5) | (procedural / no .str) |
| 18 | HAT_EF_Valhalla_Idol | resourceFileName | efst_valhalla_idol\odl2.str |
| 19 | HAT_EF_Angel_Stairs | resourceFileName | cloudh.str |
| 20 | HAT_EF_Glow_Of_New_Year | resourceFileName | efst_GlowOfNewYear\halo.str |
| 21 | HAT_EF_BOTTOM_FORTUNEKISS | effectID 293 (0x125) | (procedural / no .str) |
| 22 | HAT_EF_PINKBODY | effectID 396 (0x18c) | (procedural / no .str) |
| 23 | HAT_EF_DOUBLEGUMGANG | effectID 418 (0x1a2) | (procedural / no .str) |
| 24 | HAT_EF_GIANTBODY | effectID 423 (0x1a7) | (procedural / no .str) |
| 25 | HAT_EF_GREEN99_6 | effectID 680 (0x2a8) | (procedural / no .str) |
| 26 | HAT_EF_CIRCLEPOWER | effectID 1122 (0x462) | (procedural / no .str) |
| 27 | HAT_EF_BOTTOM_BLOODYLUST | effectID 829 (0x33d) | (procedural / no .str) |
| 28 | HAT_EF_WATER_BELOW | effectID 838 (0x346) | (procedural / no .str) |
| 29 | HAT_EF_LEVEL99_150 | effectID 881 (0x371) | (procedural / no .str) |
| 30 | HAT_EF_YELLOWFLY3 | effectID 946 (0x3b2) | (procedural / no .str) |
| 31 | HAT_EF_KAGEMUSYA | effectID 1004 (0x3ec) | (procedural / no .str) |
| 32 | HAT_EF_CHERRYBLOSSOM | effectID 1013 (0x3f5) | (procedural / no .str) |
| 33 | HAT_EF_STRANGELIGHTS | resourceFileName | efst_STRANGELIGHTS\strangelights.str |
| 34 | HAT_EF_WL_TELEKINESIS_INTENSE | effectID 1048 (0x418) | (procedural / no .str) |
| 35 | HAT_EF_AB_OFFERTORIUM_RING | effectID 1057 (0x421) | (procedural / no .str) |
| 36 | HAT_EF_WHITEBODY2 | effectID 1065 (0x429) | (procedural / no .str) |
| 37 | HAT_EF_SAKURA | effectID 163 (0xa3) | (procedural / no .str) |
| 38 | HAT_EF_CLOUD2 | effectID 230 (0xe6) | (procedural / no .str) |
| 39 | HAT_EF_Feather_Fluttering | resourceFileName | efst_feather_fluttering\feath.str |
| 40 | HAT_EF_Camellia_Hair_Pin | resourceFileName | efst_flowersmoke\flowersmoke.str |
| 41 | HAT_EF_Jp_Ev_Effect01 | effectID 293 (0x125) | (procedural / no .str) |
| 42 | HAT_EF_Jp_Ev_Effect02 | effectID 293 (0x125) | (procedural / no .str) |
| 43 | HAT_EF_Jp_Ev_Effect03 | effectID 293 (0x125) | (procedural / no .str) |
| 44 | HAT_EF_Floral_Waltz | resourceFileName | efst_Floral_Waltz\Floral_Waltz.str |
| 45 | HAT_EF_magical_feather | resourceFileName | efst_magical_feather\magical_feather.str |
| 46 | HAT_EF_HAT_EFFECT | effectID 1012 (0x3f4) | cloudh.str |
| 47 | HAT_EF_BAKURETSU_HADOU | effectID 1130 (0x46a) | (procedural / no .str) |
| 48 | HAT_EF_gold_shower | resourceFileName | efst_Gold_Shower\coin2.str |
| 49 | HAT_EF_WHITEBODY | effectID 1131 (0x46b) | (procedural / no .str) |
| 50 | HAT_EF_WATER_BELOW2 | effectID 838 (0x346) | (procedural / no .str) |
| 51 | HAT_EF_firework | resourceFileName | efst_firework\firework.str |
| 52 | HAT_EF_Return_TW_1st_Hat | resourceFileName | EFST_Return_TW_1st_Hat\tensi3.str |
| 53 | HAT_EF_C_FlutterButterfly_BL | resourceFileName | efst_FlutterButterfly_BL\Flutter_Butterfly.str |
| 54 | HAT_EF_Qscaraba | resourceFileName | EFST_Qscaraba\Qscaraba.str |
| 55 | HAT_EF_FSTONE | resourceFileName | efst_fstone\stoneofint.str |
| 56 | HAT_EF_Magiccircle | resourceFileName | efst_Magiccircle\mc.str |
| 57 | HAT_EF_Brysinggamen | effectID 1193 (0x4a9) | (procedural / no .str) |
| 58 | HAT_EF_Magingiorde | effectID 1194 (0x4aa) | (procedural / no .str) |
| 59 | HAT_EF_LEVEL99_RED | effectID 1164 (0x48c) | (procedural / no .str) |
| 60 | HAT_EF_LEVEL99_ULTRAMARINE | effectID 1165 (0x48d) | (procedural / no .str) |
| 61 | HAT_EF_LEVEL99_CYAN | effectID 1166 (0x48e) | (procedural / no .str) |
| 62 | HAT_EF_LEVEL99_LIME | effectID 1167 (0x48f) | (procedural / no .str) |
| 63 | HAT_EF_LEVEL99_VIOLET | effectID 1168 (0x490) | (procedural / no .str) |
| 64 | HAT_EF_LEVEL99_LILAC | effectID 1169 (0x491) | (procedural / no .str) |
| 65 | HAT_EF_LEVEL99_SUN_ORANGE | effectID 1170 (0x492) | (procedural / no .str) |
| 66 | HAT_EF_LEVEL99_DEEP_PINK | effectID 1171 (0x493) | (procedural / no .str) |
| 67 | HAT_EF_LEVEL99_BLACK | effectID 1172 (0x494) | (procedural / no .str) |
| 68 | HAT_EF_LEVEL99_WHITE | effectID 1173 (0x495) | (procedural / no .str) |
| 69 | HAT_EF_LEVEL160_RED | effectID 1174 (0x496) | (procedural / no .str) |
| 70 | HAT_EF_LEVEL160_ULTRAMARINE | effectID 1175 (0x497) | (procedural / no .str) |
| 71 | HAT_EF_LEVEL160_CYAN | effectID 1176 (0x498) | (procedural / no .str) |
| 72 | HAT_EF_LEVEL160_LIME | effectID 1177 (0x499) | (procedural / no .str) |
| 73 | HAT_EF_LEVEL160_VIOLET | effectID 1178 (0x49a) | (procedural / no .str) |
| 74 | HAT_EF_LEVEL160_LILAC | effectID 1179 (0x49b) | (procedural / no .str) |
| 75 | HAT_EF_LEVEL160_SUN_ORANGE | effectID 1180 (0x49c) | (procedural / no .str) |
| 76 | HAT_EF_LEVEL160_DEEP_PINK | effectID 1181 (0x49d) | (procedural / no .str) |
| 77 | HAT_EF_LEVEL160_BLACK | effectID 1182 (0x49e) | (procedural / no .str) |
| 78 | HAT_EF_LEVEL160_WHITE | effectID 1183 (0x49f) | (procedural / no .str) |
| 79 | HAT_EF_Full_BloomCherry_Tree | resourceFileName | efst_Full_BloomCherry_Tree\Full_BloomCherry_Tree.str |
| 80 | HAT_EF_C_Blessings_Of_Soul | resourceFileName | efst_C_Blessings_Of_Soul\blessingsofsoul.str |
| 81 | HAT_EF_ManyStars | resourceFileName | efst_ManyStars\hikariga.str |
| 82 | HAT_EF_SUBJECT_AURA_GOLD | effectID 1211 (0x4bb) | (procedural / no .str) |
| 83 | HAT_EF_SUBJECT_AURA_WHITE | effectID 1212 (0x4bc) | (procedural / no .str) |
| 84 | HAT_EF_SUBJECT_AURA_RED | effectID 1213 (0x4bd) | (procedural / no .str) |
| 85 | HAT_EF_C_Shining_Angel_Wing | resourceFileName | efst_C_Shining_Angel_Wing\C_Shining_Angel_Wing.str |
| 86 | HAT_EF_Magic_Star_TW | resourceFileName | efst_Mstone\stoneofint2.str |
| 87 | HAT_EF_Digital_Space | effectID 1240 (0x4d8) | (procedural / no .str) |
| 88 | HAT_EF_Sleipnir | effectID 1241 (0x4d9) | (procedural / no .str) |
| 89 | HAT_EF_C_Maple_Which_Falls_Rd | resourceFileName | efst_C_Maple_Which_Falls_Rd\C_Maple_Which_Falls_Rd.str |
| 90 | HAT_EF_MagiccircleRainbow | resourceFileName | efst_MagiccircleRainbow\mcr.str |
| 91 | HAT_EF_SnowFlake_Tiara | resourceFileName | efst_SnowFlake_Tiara\nnnaaa.str |
| 92 | HAT_EF_Midgarts_Glory | resourceFileName | efst_Midgarts_Glory\halo_2.str |
| 93 | HAT_EF_LEVEL99_TIGER | effectID 1291 (0x50b) | (procedural / no .str) |
| 94 | HAT_EF_LEVEL160_TIGER | effectID 1292 (0x50c) | (procedural / no .str) |
| 95 | HAT_EF_FluffyWing | resourceFileName | efst_FluffyWing\ypen.str |
| 96 | HAT_EF_C_Ghost_Effect | resourceFileName | efst_C_Ghost_Effect\C_Ghost_Effect.str |
| 97 | HAT_EF_C_Popping_Poring_Aura | resourceFileName | efst_C_Popping_Poring_Aura\C_Popping_Poring_Aura.str |
| 98 | HAT_EF_ResonateTaego | resourceFileName | efst_ResonateTaego\youmei.str |
| 99 | HAT_EF_99LV_Rune_Red | effectID 1325 (0x52d) | (procedural / no .str) |
| 100 | HAT_EF_99LV_Royal_Guard_Blue | effectID 1326 (0x52e) | (procedural / no .str) |
| 101 | HAT_EF_99LV_Warlock_Violet | effectID 1327 (0x52f) | (procedural / no .str) |
| 102 | HAT_EF_99LV_Sorcerer_LBlue | effectID 1328 (0x530) | (procedural / no .str) |
| 103 | HAT_EF_99LV_Ranger_Green | effectID 1329 (0x531) | (procedural / no .str) |
| 104 | HAT_EF_99LV_Minstrel_Pink | effectID 1330 (0x532) | (procedural / no .str) |
| 105 | HAT_EF_99LV_Archbishop_White | effectID 1331 (0x533) | (procedural / no .str) |
| 106 | HAT_EF_99LV_Guill_Silver | effectID 1332 (0x534) | (procedural / no .str) |
| 107 | HAT_EF_99LV_ShadowC_Black | effectID 1333 (0x535) | (procedural / no .str) |
| 108 | HAT_EF_99LV_Mechanic_Gold | effectID 1334 (0x536) | (procedural / no .str) |
| 109 | HAT_EF_99LV_Genetic_YGreen | effectID 1335 (0x537) | (procedural / no .str) |
| 110 | HAT_EF_160LV_Rune_Red | effectID 1336 (0x538) | (procedural / no .str) |
| 111 | HAT_EF_160LV_Royal_G_Blue | effectID 1337 (0x539) | (procedural / no .str) |
| 112 | HAT_EF_160LV_Warlock_Violet | effectID 1338 (0x53a) | (procedural / no .str) |
| 113 | HAT_EF_160LV_Sorcerer_LBlue | effectID 1339 (0x53b) | (procedural / no .str) |
| 114 | HAT_EF_160LV_Ranger_Green | effectID 1340 (0x53c) | (procedural / no .str) |
| 115 | HAT_EF_160LV_Minstrel_Pink | effectID 1341 (0x53d) | (procedural / no .str) |
| 116 | HAT_EF_160LV_Archb_White | effectID 1342 (0x53e) | (procedural / no .str) |
| 117 | HAT_EF_160LV_Guill_Silver | effectID 1343 (0x53f) | (procedural / no .str) |
| 118 | HAT_EF_160LV_ShadowC_Black | effectID 1344 (0x540) | (procedural / no .str) |
| 119 | HAT_EF_160LV_Mechanic_Gold | effectID 1345 (0x541) | (procedural / no .str) |
| 120 | HAT_EF_160LV_Genetic_YGreen | effectID 1346 (0x542) | (procedural / no .str) |
| 121 | HAT_EF_WATER_BELOW3 | resourceFileName | efst_Waterfield\waterfield2.str |
| 122 | HAT_EF_WATER_BELOW4 | resourceFileName | efst_Waterfield2\waterfield3.str |
| 123 | HAT_EF_C_Valkyrie_Wing | effectID 1377 (0x561) | (procedural / no .str) |
| 124 | HAT_EF_2019RTC_CeleAura_TW | resourceFileName | efst_2019RTC_CeleAura_TW\poporingb.str |
| 125 | HAT_EF_2019RTC1ST_TW | resourceFileName | efst_2019RTC1ST_TW\kporingbg.str |
| 126 | HAT_EF_2019RTC2ST_TW | resourceFileName | efst_2019RTC2ST_TW\angelpo.str |
| 127 | HAT_EF_2019RTC3ST_TW | resourceFileName | efst_2019RTC3ST_TW\dringbg.str |
| 128 | HAT_EF_CONS_OF_WIND | effectID 1531 (0x5fb) | (procedural / no .str) |
| 129 | HAT_EF_Maple_Falls | resourceFileName | efst_maple_falls\maple_falls.str |
| 130 | HAT_EF_BJ_HeadsetB | resourceFileName | BJ_HeadsetB\rhythmageruyo.str |
| 131 | HAT_EF_VIP_Hair | resourceFileName | efst_VIP_Hair\rainbow_2.str |
| 132 | HAT_EF_C_Magic_Heir_TW | resourceFileName | efst_C_Magic_Heir_TW\moonstar2.str |
| 133 | HAT_EF_C_Sudden_Wealth_TW | resourceFileName | efst_C_Sudden_Wealth_TW\wonbo.str |
| 134 | HAT_EF_C_Romance_Rose_TW | resourceFileName | efst_C_Romance_Rose_TW\losttime.str |
| 135 | HAT_EF_C_Disapear_Time_TW | resourceFileName | efst_C_Disapear_Time_TW\cdhs.str |
| 136 | HAT_EF_2020RTC_01 | resourceFileName | 2020RTC_01\mcgold.str |
| 137 | HAT_EF_2020RTC_02 | resourceFileName | 2020RTC_02\mcblack.str |
| 138 | HAT_EF_2020RTC_03 | resourceFileName | 2020RTC_03\mcred.str |
| 139 | HAT_EF_C_2020RTC_Imp_TW | resourceFileName | C_2020RTC_Imp_TW\mc.str |
| 140 | HAT_EF_SUBJECT_AURA_BLACK | effectID 2281 (0x8e9) | (procedural / no .str) |
| 141 | HAT_EF_2020RTC_EFFECT_01 | effectID 2281 (0x8e9) | (procedural / no .str) |
| 142 | HAT_EF_2020RTC_EFFECT_02 | effectID 2281 (0x8e9) | (procedural / no .str) |
| 143 | HAT_EF_2020RTC_EFFECT_03 | effectID 2281 (0x8e9) | (procedural / no .str) |
| 144 | HAT_EF_99LV_STAR_E_MBLUE | effectID 2281 (0x8e9) | (procedural / no .str) |
| 145 | HAT_EF_160LV_STAR_E_MBLUE | effectID 2282 (0x8ea) | (procedural / no .str) |
| 146 | HAT_EF_99LV_SOUL_R_GRAY | effectID 2283 (0x8eb) | (procedural / no .str) |
| 147 | HAT_EF_160LV_SOUL_R_GRAY | effectID 2284 (0x8ec) | (procedural / no .str) |
| 148 | HAT_EF_GearWheel | resourceFileName | C_Rotating_Gears\gearwheel.str |
| 149 | HAT_EF_GIFT_OF_SNOW | resourceFileName | efst_gift_of_snow\gift_of_snow.str |
| 150 | HAT_EF_Snow_Powder | resourceFileName | efst_Snow_Powder\ssnnnn2.str |
| 151 | HAT_EF_Falling_Snow | resourceFileName | efst_Falling_Snow\Falling_Snow.str |
| 152 | HAT_EF_C_Phigasia_Scarf_EXE | resourceFileName | efst_C_Phigasia_Scarf_EXE\singa.str |
| 153 | HAT_EF_C_Kyel_hyre_Ulti_TW | resourceFileName | EFST_Kyel_hyre_Ulti_TW\tentaKaiser.str |
| 154 | HAT_EF_C_Master | resourceFileName | efst_C_Master\13123123.str |
| 155 | HAT_EF_C_Time_Accessory | resourceFileName | efst_time_accessory\time_accessory.str |
| 156 | HAT_EF_C_Helm_Of_Ra | resourceFileName | C_Helm_Of_Ra\HelmOfSun3.str |
| 157 | HAT_EF_C_2021RTC_Headset_TW | resourceFileName | C_2021RTC_Headset_TW\hd.str |
| 158 | HAT_EF_C_MoonStar_Accessory | resourceFileName | moonstar.str |
| 159 | HAT_EF_BLACK_THUNDER | effectID 2346 (0x92a) | (procedural / no .str) |
| 160 | HAT_EF_BLACK_THUNDER_DARK | effectID 2347 (0x92b) | (procedural / no .str) |
| 161 | HAT_EF_C_Released_Ground | resourceFileName | C_Released_Ground\ki.str |
| 162 | HAT_EF_C_Samba_Carnival | resourceFileName | efst_C_Samba_Carnival\twinklestar.str |
| 163 | HAT_EF_POISON_MASTER | effectID 2310 (0x906) | (procedural / no .str) |
| 164 | HAT_EF_C_Swirling_Flame | resourceFileName | C_Swirling_Flame\vortexf2.str |
| 165 | HAT_EF_C_2021RTC_Headset_1_TW | resourceFileName | C_2021RTC_Headset_1_TW\hd.str |
| 166 | HAT_EF_C_2021RTC_Headset_2_TW | resourceFileName | C_2021RTC_Headset_2_TW\hd.str |
| 167 | HAT_EF_C_2021RTC_Headset_3_TW | resourceFileName | C_2021RTC_Headset_3_TW\hd.str |
| 168 | HAT_EF_SUBJECT_AURA_WHITE_ALPHA | effectID 2370 (0x942) | (procedural / no .str) |
| 169 | HAT_EF_GC_DARKCROW | effectID 1184 (0x4a0) | (procedural / no .str) |
| 170 | HAT_EF_DIABOLUS_RING | effectID 2309 (0x905) | (procedural / no .str) |
| 171 | HAT_EF_Magiccircle_Blue_TW | resourceFileName | efst_magiccircle_Blue_TW\bluemc.str |
| 172 | HAT_EF_C_Disapear_Time_TW_2 | resourceFileName | efst_C_Disapear_Time_TW\cdhs.str |
| 173 | HAT_EF_C_Melody_Wing | resourceFileName | C_Melody_Wing\notetama.str |
| 174 | HAT_EF_C_Spot_Light | resourceFileName | C_Spot_Light\Spotlight.str |
| 175 | HAT_EF_C_Astra_Blessing | resourceFileName | efst_C_Astra_Blessing\astra.str |
| 176 | HAT_EF_efst_C_20th_Anniversary_Hat | resourceFileName | efst_C_20th_Anniversary_Hat\20th_f.str |
| 177 | HAT_EF_SUBJECT_AURA_NAVY | effectID 2301 (0x8fd) | (procedural / no .str) |
| 178 | HAT_EF_20th_Scarf_J | resourceFileName | efst_20th_Scarf_J\singa.str |
| 179 | HAT_EF_Ghost_Fire | resourceFileName | Efst_Ghost_Fire\strangelights2.str |
| 180 | HAT_EF_SERPENT_SHADOW | effectID 2394 (0x95a) | (procedural / no .str) |
| 181 | HAT_EF_C_1st_Evt_Hat_MSP | resourceFileName | efst_C_1st_Evt_Hat_MSP\firework.str |
| 182 | HAT_EF_C_1st_Evt_Balloon_MSP | resourceFileName | efst_C_1st_Evt_Balloon_MSP\ggh1st.str |
| 183 | HAT_EF_rabbit_aura | resourceFileName | efst_rabbit_aura\toto.str |
| 184 | HAT_EF_alice_tea | resourceFileName | efst_alice_tea\Alice02.str |
| 185 | HAT_EF_C_Dark_Lord_Cloak | resourceFileName | efst_C_Dark_Lord_Cloak\darklordcloak.str |
| 186 | HAT_EF_c_sakura_fubuki | resourceFileName | efst_c_sakura_fubuki\sakura_fubuki.str |
| 187 | HAT_EF_C_Dark_Lord_Manteau | resourceFileName | C_Dark_Lord_Manteau\darklordcloak02.str |
| 188 | HAT_EF_decoration_of_music | resourceFileName | efst_decoration_of_music\note_1.str |
| 189 | HAT_EF_2023RTC_S_Robe1 | resourceFileName | 2023RTC_S_Robe1\gold1.str |
| 190 | HAT_EF_2023RTC_S_Robe2 | resourceFileName | 2023RTC_S_Robe2\Silverlightning.str |
| 191 | HAT_EF_2023RTC_S_Robe3 | resourceFileName | 2023RTC_S_Robe3\bronz.str |
| 192 | HAT_EF_C_Consecrate_F_Aureola | resourceFileName | efst_C_Consecrate_F_Aureola\ConsecrateAureola.str |
| 193 | HAT_EF_C_Bulb_Wreath | resourceFileName | efst_C_Bulb_Wreath\TFireworks.str |
| 194 | HAT_EF_MD_Hol_Barrier1 | resourceFileName | efst_MD_Hol_Barrier\1mon.str |
| 195 | HAT_EF_MD_Hol_Barrier2 | resourceFileName | efst_MD_Hol_Barrier\2mon.str |
| 196 | HAT_EF_MD_Hol_Barrier3 | resourceFileName | efst_MD_Hol_Barrier\3mon.str |
| 197 | HAT_EF_MD_Hol_Barrier4 | resourceFileName | efst_MD_Hol_Barrier\4mon.str |
| 198 | HAT_EF_MD_Hol_Barrier5 | resourceFileName | efst_MD_Hol_Barrier\5mon.str |
| 199 | HAT_EF_MD_Hol_Barrier6 | resourceFileName | efst_MD_Hol_Barrier\6mon.str |
| 200 | HAT_EF_MD_Hol_Barrier7 | resourceFileName | efst_MD_Hol_Barrier\7mon.str |
| 201 | HAT_EF_MD_Hol_Barrier8 | resourceFileName | efst_MD_Hol_Barrier\8mon.str |
| 202 | HAT_EF_MD_Hol_Barrier9 | resourceFileName | efst_MD_Hol_Barrier\9mon.str |
| 203 | HAT_EF_MD_Hol_Barrier10 | resourceFileName | efst_MD_Hol_Barrier\10mon.str |
| 204 | HAT_EF_MD_Hol_Barrier11 | resourceFileName | efst_MD_Hol_Barrier\11mon.str |
| 205 | HAT_EF_MD_Hol_Barrier12 | resourceFileName | efst_MD_Hol_Barrier\12mon.str |
| 206 | HAT_EF_MD_Hol_Barrier13 | resourceFileName | efst_MD_Hol_Barrier\13mon.str |
| 207 | HAT_EF_MD_Hol_Barrier14 | resourceFileName | efst_MD_Hol_Barrier\14mon.str |
| 208 | HAT_EF_MD_Hol_Barrier15 | resourceFileName | efst_MD_Hol_Barrier\15mon.str |
| 209 | HAT_EF_MD_Hol_Barrier16 | resourceFileName | efst_MD_Hol_Barrier\16mon.str |
| 210 | HAT_EF_MD_Hol_Barrier17 | resourceFileName | efst_MD_Hol_Barrier\17mon.str |
| 211 | HAT_EF_MD_Hol_Barrier18 | resourceFileName | efst_MD_Hol_Barrier\18mon.str |
| 212 | HAT_EF_MD_Hol_Barrier19 | resourceFileName | efst_MD_Hol_Barrier\19mon.str |
| 213 | HAT_EF_MD_Hol_Barrier20 | resourceFileName | efst_MD_Hol_Barrier\20mon.str |
| 214 | HAT_EF_C_Fluttering_Haze | resourceFileName | efst_C_Fluttering_Haze\skaura33.str |
| 215 | HAT_EF_efst_cinnamon | resourceFileName | efst_cinnamon\san.str |
| 216 | HAT_EF_Autumn_Full_Moon | resourceFileName | efst_Autumn_Full_Moon\han.str |
| 217 | HAT_EF_Niflheim_Night_Sky | resourceFileName | efst_Niflheim_Night_Sky\halloween.str |
| 218 | HAT_EF_C_ROS2023_Cape_1 | resourceFileName | efst_C_ROS2023_Cape_1\ros2023_1st.str |
| 219 | HAT_EF_black_thunder | resourceFileName | efst_black_thunder\ros2023_f.str |
| 220 | HAT_EF_C_ROS2023_Cape_2 | resourceFileName | efst_C_ROS2023_Cape_2\ros2023_2nd.str |
| 221 | HAT_EF_C_15th_Nov_Helmet | resourceFileName | efst_C_15th_Nov_Helmet\tai.str |
| 222 | HAT_EF_Cosmic_Connection | resourceFileName | efst_Cosmic_Connection\strbright.str |
| 223 | HAT_EF_C_Baby_Gloom | resourceFileName | efst_C_Baby_Gloom\gloom.str |
| 224 | HAT_EF_WinterNightBells | resourceFileName | efst_WinterNightBells\christmasx4.str |
| 225 | HAT_EF_NightSkyOfRutie | resourceFileName | efst_NightSkyOfRutie\christmasx3.str |
| 231 | HAT_EF_RAINBOW_POISON_MASTER | effectID 2413 (0x96d) | (procedural / no .str) |
| 232 | HAT_EF_C_Ancient_Rune | resourceFileName | efst_C_Ancient_Rune\sangorunic1.str |
| 233 | HAT_EF_C_Dragon_Green_Aura | resourceFileName | efst_C_Dragon_Green_Aura\gryoumei.str |
| 234 | HAT_EF_C_Dragon_Red_Aura | resourceFileName | efst_C_Dragon_Red_Aura\redyoumei.str |
| 235 | HAT_EF_C_Dragon_Yellow_Aura | resourceFileName | efst_C_Dragon_Yellow_Aura\redyoumei2.str |
| 236 | HAT_EF_Interdimensional_Rift | resourceFileName | efst_Interdimensional_Rift\Blackhole.str |
| 237 | HAT_EF_C_CLB_SS_LL | resourceFileName | efst_C_CLB_SS_LL\bbcat.str |
| 238 | HAT_EF_Vacation | resourceFileName | efst_Vacation\vacation.str |
| 239 | HAT_EF_C_FH_Lostwing | resourceFileName | efst_C_FH_Lostwing\fhlostwing.str |
| 241 | HAT_EF_C_Auspicloud | resourceFileName | C_Auspicloud\sucloud.str |
| 242 | HAT_EF_AURA_OF_GHOST_S | effectID 2424 (0x978) | (procedural / no .str) |
| 243 | HAT_EF_C_ROS2024_Wing_1 | resourceFileName | efst_C_ROS2024_Wing_1\2024win.str |
| 247 | HAT_EF_Atque_Poenitentia | effectID 2428 (0x97c) | (procedural / no .str) |
| 248 | HAT_EF_Perm_Frost_Oblivion | effectID 2429 (0x97d) | (procedural / no .str) |
| 249 | HAT_EF_Atque_Poenitentia2 | resourceFileName | efst_Atque_Poenitentia2\caeffect.str |
| 250 | HAT_EF_GUIDE_OF_DEAD_TEXT | effectID 2430 (0x97e) | (procedural / no .str) |
| 251 | HAT_EF_MEDJED_TEXT | effectID 2431 (0x97f) | (procedural / no .str) |
| 252 | HAT_EF_InkPainting_Day | resourceFileName | efst_InkPainting_Day\bushinkpaint1.str |
| 253 | HAT_EF_InkPainting_Night | resourceFileName | efst_InkPainting_Night\bushinkpaint2.str |
| 254 | HAT_EF_Kung_Fu_Panda | resourceFileName | efst_Kung_Fu_Panda\redyoumei2.str |
| 255 | HAT_EF_C_MgSgPh_Potarl | resourceFileName | efst_c_mysgph_portal\potal.str |
| 256 | HAT_EF_C_Iguazu_Falls | resourceFileName | efst_C_Iguazu_Falls\Waterfall.str |
| 262 | HAT_EF_hanmac_munch | resourceFileName | efst_hanmac_munch\munch.str |
| 266 | HAT_EF_C_Over_Cloud | resourceFileName | efst_C_Over_Cloud\drgbgeffect.str |
| 267 | HAT_EF_C_Aurora_On_Clouds | resourceFileName | efst_C_Aurora_On_Clouds\drgauroraeffect.str |
| 268 | HAT_EF_ROS_RedSpirit | resourceFileName | efst_ROS_RedSpirit\kiaura1.str |
| 269 | HAT_EF_ROS_BlueSpirit | resourceFileName | efst_ROS_BlueSpirit\kiaura2.str |
| 270 | HAT_EF_Divine_Sky_Invite | resourceFileName | efst_Divine_Sky_Invite\Midgard2.str |
| 274 | HAT_EF_C_Nightmare_Chain | resourceFileName | efst_C_Nightmare_Chain\rrrooo2.str |
| 275 | HAT_EF_C_Spot_Mike | resourceFileName | C_Spot_Mike\Spotlight2.str |
| 276 | HAT_EF_C_Spot_Flower | resourceFileName | C_Spot_Flower\Spotlight1.str |
| 277 | HAT_EF_C_2025RosFesta | effectID 2443 (0x98b) | (procedural / no .str) |
| 278 | HAT_EF_Golden_Aura_TW | resourceFileName | efst_Golden_Aura_TW\fhlostwing2.str |
| 288 | HAT_EF_C_CLB_GAT_doc | resourceFileName | efst_C_CLB_GAT_doc\Gatchamanvi.str |
