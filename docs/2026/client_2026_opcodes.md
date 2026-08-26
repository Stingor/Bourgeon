# Opcodes du client 2026-07-07

Relevé du 2026-08-26 — extraction statique de `PacketLenTable_Fill` (`0x00A9C4D0`),
1577 entrées. Méthode et diff : [packet_len_diff.md](packet_len_diff.md).

Colonnes : **len** = longueur déclarée par le client, opcode compris ; `-1` = paquet
à longueur VARIABLE (elle se lit dans les 2 octets suivant l'opcode).
**srv** = ce que déclare Moonlight (`clif_packetdb.hpp`) ; vide = opcode que le
serveur n'utilise pas. **Δ** = `CHANGE` si la longueur diffère du client 2025-07-16,
`nouveau` si l'opcode n'existait pas.

## Résumé

| | |
|---|---|
| opcodes du client 2026 | 1577 |
| … dont utilisés par Moonlight | 495 |
| … dont à longueur variable | 350 |
| nouveaux depuis 2025 | 60 |
| longueur changée depuis 2025 | 26 |
| disparus depuis 2025 | 5 |

🔴 **Aucun opcode utilisé par Moonlight ne change de longueur.**
Seul point d'attention : `0x02F7` `ZC_UPDATE_GDID`, présent en 2025 (47) et
ABSENT en 2026 — il serait donc traité comme VARIABLE. Émis en `clif.cpp:13177`.

## Opcodes utilisés par Moonlight

| opcode | len | srv | nom | Δ |
|---|---|---|---|---|
| `0x0064` | 55 | 55 |  |  |
| `0x0065` | 17 | 17 |  |  |
| `0x0067` | 37 | 37 |  |  |
| `0x0068` | 46 | 46 |  |  |
| `0x0069` | -1 | -1 |  |  |
| `0x006A` | 23 | 23 |  |  |
| `0x006B` | -1 | -1 |  |  |
| `0x006C` | 3 | 3 |  |  |
| `0x006E` | 3 | 3 |  |  |
| `0x006F` | 2 | 2 |  |  |
| `0x0071` | 28 | 28 |  |  |
| `0x0075` | -1 | -1 |  |  |
| `0x0076` | 9 | 9 |  |  |
| `0x0077` | 5 | 5 |  |  |
| `0x0079` | 53 | 53 |  |  |
| `0x007B` | 60 | 60 |  |  |
| `0x007D` | 2 | 2 | CZ_LoadEndAck |  |
| `0x0082` | 2 | 2 |  |  |
| `0x0083` | 2 | 2 |  |  |
| `0x008B` | 23 | 2 |  |  |
| `0x008D` | -1 | -1 |  |  |
| `0x008E` | -1 | -1 |  |  |
| `0x0093` | 2 | 2 |  |  |
| `0x0096` | -1 | -1 | CZ_WisMessage |  |
| `0x009E` | 19 | 17 |  |  |
| `0x00AB` | 4 | 4 | CZ_UnequipItem |  |
| `0x00AE` | -1 | -1 |  |  |
| `0x00B2` | 3 | 3 | CZ_Restart |  |
| `0x00B8` | 7 | 7 | CZ_NpcSelectMenu |  |
| `0x00B9` | 6 | 6 | CZ_NpcNextClicked |  |
| `0x00BA` | 2 | 2 |  |  |
| `0x00BB` | 5 | 5 | CZ_StatusUp |  |
| `0x00C1` | 2 | 2 | CZ_HowManyConnections |  |
| `0x00C3` | 8 | 8 |  |  |
| `0x00C6` | -1 | -1 | ZC_PC_PURCHASE_ITEMLIST |  |
| `0x00C8` | -1 | -1 | CZ_NpcBuyListSend |  |
| `0x00CA` | 3 | 3 |  |  |
| `0x00CB` | 3 | 3 |  |  |
| `0x00CC` | 6 | 6 | CZ_GMKick |  |
| `0x00CD` | 3 | 3 |  |  |
| `0x00CE` | 2 | 2 | CZ_GMKickAll |  |
| `0x00CF` | 27 | 27 | CZ_PMIgnore |  |
| `0x00D3` | 2 | 2 | CZ_PMIgnoreList |  |
| `0x00E0` | 30 | 30 | CZ_ChangeChatOwner |  |
| `0x00E2` | 26 | 26 | CZ_KickFromChat |  |
| `0x00E3` | 2 | 2 | CZ_ChatLeave |  |
| `0x00E4` | 6 | 6 | CZ_TradeRequest |  |
| `0x00E5` | 26 | 26 |  |  |
| `0x00E6` | 3 | 3 | CZ_TradeAck |  |
| `0x00EA` | 5 | 5 |  |  |
| `0x00EB` | 2 | 2 | CZ_TradeOk |  |
| `0x00ED` | 2 | 2 | CZ_TradeCancel |  |
| `0x00EF` | 2 | 2 | CZ_TradeCommit |  |
| `0x00FB` | -1 | -1 |  |  |
| `0x00FD` | 27 | 27 | ZC_PARTY_JOIN_REQ_ACK |  |
| `0x0101` | 6 | 6 |  |  |
| `0x0102` | 6 | 6 | CZ_PartyChangeOption |  |
| `0x0104` | 79 | 79 |  |  |
| `0x0108` | -1 | -1 | CZ_PartyMessage |  |
| `0x0109` | -1 | -1 | ZC_NOTIFY_CHAT_PARTY |  |
| `0x0112` | 4 | 4 | CZ_SkillUp |  |
| `0x0114` | 31 | 31 | ZC_NOTIFY_SKILL |  |
| `0x0115` | 35 | 35 | ZC_NOTIFY_SKILL_POSITION |  |
| `0x0118` | 2 | 2 | CZ_StopAttack |  |
| `0x0119` | 13 | 13 | ZC_STATE_CHANGE |  |
| `0x011D` | 2 | 2 | CZ_RequestMemo |  |
| `0x011F` | 16 | 16 |  |  |
| `0x012A` | 2 | 2 | CZ_RemoveOption |  |
| `0x012E` | 2 | 2 | CZ_CloseVending |  |
| `0x012F` | -1 | -1 | CZ_OpenVending |  |
| `0x0130` | 6 | 6 | CZ_VendingListReq |  |
| `0x0138` | 3 | 3 |  |  |
| `0x013F` | 26 | 26 | CZ_GM_Item_Monster |  |
| `0x0145` | 19 | 19 |  |  |
| `0x0147` | 39 | 39 | ZC_AUTORUN_SKILL |  |
| `0x0149` | 9 | 9 | CZ_GMReqNoChat |  |
| `0x014A` | 6 | 6 |  |  |
| `0x014D` | 2 | 2 | CZ_GuildCheckMaster |  |
| `0x014F` | 6 | 6 | CZ_GuildRequestInfo |  |
| `0x0150` | 110 | 110 |  |  |
| `0x0151` | 6 | 6 | CZ_REQ_GUILD_EMBLEM_IMG1 |  |
| `0x0152` | -1 | -1 | ZC_GUILD_EMBLEM_IMG |  |
| `0x0153` | -1 | -1 | CZ_GuildChangeEmblem |  |
| `0x0154` | -1 | -1 | ZC_MEMBERMGR_INFO |  |
| `0x0157` | 6 | 6 |  |  |
| `0x015F` | 42 | 42 |  |  |
| `0x0161` | -1 | -1 | CZ_GuildChangePositionInfo |  |
| `0x0164` | -1 | -1 |  |  |
| `0x0165` | 30 | 30 | CZ_CreateGuild |  |
| `0x0166` | -1 | -1 | ZC_POSITION_ID_NAME_INFO |  |
| `0x016C` | 43 | 43 | ZC_UPDATE_GDID |  |
| `0x016E` | 186 | 186 | CZ_GuildChangeNotice |  |
| `0x0170` | 14 | 14 | CZ_GuildRequestAlliance |  |
| `0x0172` | 10 | 10 | CZ_GuildReplyAlliance |  |
| `0x0175` | 6 | 6 |  |  |
| `0x0176` | 106 | 106 |  |  |
| `0x0177` | -1 | -1 |  |  |
| `0x017B` | -1 | -1 |  |  |
| `0x017E` | -1 | -1 | CZ_GuildMessage |  |
| `0x0180` | 6 | 6 | CZ_GuildOpposition |  |
| `0x0182` | 106 | 106 |  |  |
| `0x0183` | 10 | 10 | CZ_GuildDelAlliance |  |
| `0x0185` | 34 | 34 |  |  |
| `0x0187` | 6 | 6 |  |  |
| `0x018A` | 4 | 4 | CZ_QuitGame |  |
| `0x018B` | 4 | 4 |  |  |
| `0x0196` | 9 | 9 |  |  |
| `0x0198` | 8 | 8 | CZ_GMChangeMapType |  |
| `0x0199` | 4 | 4 |  |  |
| `0x019A` | 14 | 14 |  |  |
| `0x019D` | 6 | 6 | CZ_GMHide |  |
| `0x019F` | 6 | 6 | CZ_CatchPet |  |
| `0x01A1` | 3 | 3 | CZ_PetMenu |  |
| `0x01A3` | 7 | 5 |  |  |
| `0x01A5` | 26 | 26 | CZ_ChangePetName |  |
| `0x01A6` | -1 | -1 |  |  |
| `0x01A7` | 4 | 4 | CZ_SelectEgg |  |
| `0x01A8` | 4 | 4 |  |  |
| `0x01A9` | 6 | 6 | CZ_SendEmotion |  |
| `0x01AC` | 6 | 6 |  |  |
| `0x01AD` | -1 | -1 | ZC_MAKINGARROW_LIST |  |
| `0x01AF` | 4 | 4 | CZ_ChangeCart |  |
| `0x01B0` | 11 | 11 |  |  |
| `0x01B1` | 7 | 7 |  |  |
| `0x01B2` | -1 | -1 | CZ_OpenVending |  |
| `0x01B5` | 18 | 18 |  |  |
| `0x01B6` | 114 | 114 | ZC_GUILD_INFO |  |
| `0x01B7` | 6 | 6 |  |  |
| `0x01B8` | 3 | 3 |  |  |
| `0x01BA` | 26 | 26 | CZ_GMShift |  |
| `0x01BB` | 26 | 26 | CZ_GMShift |  |
| `0x01BC` | 26 | 26 | CZ_GMRecall |  |
| `0x01BD` | 26 | 26 | CZ_GMRecall |  |
| `0x01BE` | 2 | 2 |  |  |
| `0x01BF` | 3 | 3 |  |  |
| `0x01C0` | 2 | 2 | CZ_REQ_REMAINTIME |  |
| `0x01C1` | 14 | 14 |  |  |
| `0x01C2` | 10 | 10 |  |  |
| `0x01C3` | -1 | -1 |  |  |
| `0x01C6` | 4 | 4 |  |  |
| `0x01C7` | 2 | 2 |  |  |
| `0x01CA` | 3 | 3 |  |  |
| `0x01CB` | 9 | 9 |  |  |
| `0x01CC` | 9 | 9 |  |  |
| `0x01CF` | 28 | 28 |  |  |
| `0x01D0` | 8 | 8 | ZC_SPIRITS |  |
| `0x01D7` | 15 | 11 |  |  |
| `0x01D8` | 58 | 54 |  |  |
| `0x01D9` | 57 | 53 |  |  |
| `0x01DA` | 64 | 60 |  |  |
| `0x01DB` | 2 | 2 |  |  |
| `0x01DC` | -1 | -1 |  |  |
| `0x01DD` | 47 | 47 |  |  |
| `0x01DE` | 33 | 33 | ZC_NOTIFY_SKILL |  |
| `0x01DF` | 6 | 6 | CZ_GMReqAccountName |  |
| `0x01E0` | 30 | 30 |  |  |
| `0x01E1` | 8 | 8 | ZC_SPIRITS2 |  |
| `0x01E2` | 34 | 34 |  |  |
| `0x01E3` | 14 | 14 |  |  |
| `0x01E4` | 2 | 2 |  |  |
| `0x01E5` | 6 | 6 |  |  |
| `0x01E6` | 26 | 26 |  |  |
| `0x01E7` | 2 | 2 | CZ_NoviceDoriDori |  |
| `0x01EC` | 26 | 26 |  |  |
| `0x01ED` | 2 | 2 | CZ_NoviceExplosionSpirits |  |
| `0x01F0` | -1 | -1 |  |  |
| `0x01F1` | -1 | -1 |  |  |
| `0x01F3` | 10 | 10 |  |  |
| `0x01F6` | 34 | 34 |  |  |
| `0x01F8` | 2 | 2 |  |  |
| `0x01F9` | 6 | 6 | CZ_Adopt_request |  |
| `0x01FA` | 48 | 48 |  |  |
| `0x01FB` | 56 | 56 |  |  |
| `0x01FC` | -1 | -1 | ZC_REPAIRITEMLIST |  |
| `0x0200` | 26 | 26 |  |  |
| `0x0201` | -1 | -1 |  |  |
| `0x0203` | 10 | 10 | CZ_FriendsListRemove |  |
| `0x0204` | 18 | 18 |  |  |
| `0x0207` | 34 | 34 |  |  |
| `0x0209` | 36 | 36 |  |  |
| `0x020A` | 10 | 10 |  |  |
| `0x020D` | -1 | -1 |  |  |
| `0x0212` | 26 | 26 | CZ_GMRc |  |
| `0x0213` | 26 | 26 | CZ_Check |  |
| `0x0214` | 42 | 42 |  |  |
| `0x021E` | 6 | 6 |  |  |
| `0x021F` | 66 | 66 |  |  |
| `0x0220` | 10 | 10 |  |  |
| `0x0221` | -1 | -1 |  |  |
| `0x0222` | 6 | 6 | CZ_WeaponRefine |  |
| `0x0227` | 18 | 18 |  |  |
| `0x0228` | 18 | 18 |  |  |
| `0x0229` | 15 | 15 | ZC_STATE_CHANGE |  |
| `0x022A` | 62 | 58 |  |  |
| `0x022B` | 61 | 57 |  |  |
| `0x022F` | 7 | 5 |  |  |
| `0x0231` | 26 | 26 | CZ_ChangeHomunculusName |  |
| `0x0233` | 11 | 11 | CZ_HomAttack |  |
| `0x0234` | 6 | 6 | CZ_HomMoveToMaster |  |
| `0x023A` | 4 | 4 |  |  |
| `0x023C` | 6 | 6 |  |  |
| `0x023D` | 6 | -1 |  |  |
| `0x023F` | 2 | 2 | CZ_Mail_refreshinbox |  |
| `0x0241` | 6 | 6 | CZ_Mail_read |  |
| `0x0242` | -1 | -1 |  |  |
| `0x0243` | 6 | 6 | CZ_Mail_delete |  |
| `0x0244` | 6 | 6 | CZ_Mail_getattach |  |
| `0x0246` | 4 | 4 | CZ_Mail_winopen |  |
| `0x0247` | 8 | 8 | CZ_Mail_setattach |  |
| `0x0249` | 3 | 3 |  |  |
| `0x024A` | 70 | 70 |  |  |
| `0x024B` | 4 | 4 | CZ_Auction_cancelreg |  |
| `0x024C` | 8 | 8 | CZ_Auction_setitem |  |
| `0x024D` | 12 | 14 |  |  |
| `0x0250` | 3 | 3 |  |  |
| `0x0252` | -1 | -1 |  |  |
| `0x0253` | 3 | 3 |  |  |
| `0x0254` | 3 | 3 | CZ_FeelSaveOk |  |
| `0x0255` | 5 | 5 |  |  |
| `0x0257` | 8 | 8 |  |  |
| `0x025C` | 4 | 4 | CZ_Auction_buysell |  |
| `0x025D` | 6 | 6 | CZ_Auction_close |  |
| `0x025E` | 4 | 4 |  |  |
| `0x025F` | 6 | 6 |  |  |
| `0x0260` | 6 | 6 |  |  |
| `0x0261` | 11 | 11 |  |  |
| `0x0262` | 11 | 11 |  |  |
| `0x0263` | 11 | 11 |  |  |
| `0x0264` | 20 | 20 |  |  |
| `0x0265` | 20 | 20 |  |  |
| `0x0266` | 30 | 30 |  |  |
| `0x0267` | 4 | 4 |  |  |
| `0x0268` | 4 | 4 |  |  |
| `0x0269` | 4 | 4 |  |  |
| `0x026A` | 4 | 4 |  |  |
| `0x026B` | 4 | 4 |  |  |
| `0x026C` | 4 | 4 |  |  |
| `0x026D` | 4 | 4 |  |  |
| `0x026F` | 2 | 2 |  |  |
| `0x0270` | 2 | 2 |  |  |
| `0x0272` | 44 | 44 |  |  |
| `0x0274` | 8 | 8 |  |  |
| `0x0277` | 84 | 84 |  |  |
| `0x0278` | 2 | 2 |  |  |
| `0x0279` | 2 | 2 |  |  |
| `0x027A` | -1 | -1 |  |  |
| `0x027B` | 14 | 14 |  |  |
| `0x027C` | 60 | 60 |  |  |
| `0x027D` | 62 | 62 |  |  |
| `0x027F` | 8 | 8 |  |  |
| `0x0280` | 12 | 12 |  |  |
| `0x0282` | 284 | 284 |  |  |
| `0x0283` | 6 | 6 |  |  |
| `0x0284` | 14 | 14 | ZC_NOTIFY_EFFECT3 |  |
| `0x0285` | 6 | 6 |  |  |
| `0x0286` | 4 | 4 |  |  |
| `0x0287` | -1 | -1 |  |  |
| `0x028B` | -1 | -1 |  |  |
| `0x028C` | 46 | 46 |  |  |
| `0x028D` | 34 | 34 |  |  |
| `0x028E` | 4 | 4 |  |  |
| `0x028F` | 6 | 6 |  |  |
| `0x0290` | 4 | 4 |  |  |
| `0x0292` | 2 | 2 | CZ_AutoRevive |  |
| `0x0293` | 70 | 70 |  |  |
| `0x0294` | 10 | 10 |  |  |
| `0x029C` | 66 | 66 |  |  |
| `0x029D` | -1 | -1 |  |  |
| `0x029E` | 11 | 11 |  |  |
| `0x029F` | 3 | 3 | CZ_mercenary_action |  |
| `0x02A2` | 8 | 8 |  |  |
| `0x02A5` | 8 | 8 |  |  |
| `0x02AA` | 4 | 4 |  |  |
| `0x02AB` | 36 | 36 |  |  |
| `0x02AC` | 6 | 6 |  |  |
| `0x02AD` | 8 | 8 |  |  |
| `0x02B0` | 85 | 85 |  |  |
| `0x02B1` | -1 | -1 |  |  |
| `0x02B2` | -1 | -1 |  |  |
| `0x02B3` | 107 | 107 |  |  |
| `0x02B4` | 6 | 6 |  |  |
| `0x02B5` | -1 | -1 |  |  |
| `0x02B7` | 7 | 7 |  |  |
| `0x02B9` | 191 | 191 | ZC_SHORTCUT_KEY_LIST |  |
| `0x02BA` | 11 | 11 | CZ_SHORTCUT_KEY_CHANGE1 |  |
| `0x02BC` | 6 | 6 |  |  |
| `0x02C1` | -1 | -1 |  |  |
| `0x02C2` | -1 | -1 |  |  |
| `0x02C5` | 30 | 30 | ZC_PARTY_JOIN_REQ_ACK |  |
| `0x02CA` | 3 | 3 |  |  |
| `0x02CC` | 4 | 4 |  |  |
| `0x02CE` | 10 | 10 |  |  |
| `0x02CF` | 6 | 6 | CZ_MemorialDungeonCommand |  |
| `0x02D5` | 2 | 2 |  |  |
| `0x02D6` | 6 | 6 | CZ_ViewPlayerEquip |  |
| `0x02D8` | 10 | 10 | CZ_configuration |  |
| `0x02D9` | 10 | 10 |  |  |
| `0x02DB` | -1 | -1 | CZ_BattleChat |  |
| `0x02DC` | -1 | -1 |  |  |
| `0x02DD` | 32 | 32 |  |  |
| `0x02DE` | 6 | 6 |  |  |
| `0x02DF` | 36 | 36 |  |  |
| `0x02E0` | 34 | 34 | ZC_BATTLEFIELD_NOTIFY_HP |  |
| `0x02E6` | 6 | 6 |  |  |
| `0x02E7` | -1 | -1 |  |  |
| `0x02EC` | 71 | 67 |  |  |
| `0x02ED` | 63 | 59 |  |  |
| `0x02EE` | 64 | 60 |  |  |
| `0x02EF` | 8 | 8 |  |  |
| `0x02F0` | 10 | 10 |  |  |
| `0x02F1` | 2 | 2 | CZ_progressbar |  |
| `0x02F2` | 2 | 2 |  |  |
| `0x02F3` | -1 | -1 |  |  |
| `0x02F4` | 3 | -1 |  |  |
| `0x02F5` | 7 | -1 |  |  |
| `0x02F6` | 7 | -1 |  |  |
| `0x02F8` | -1 | -1 |  | nouveau |
| `0x02F9` | -1 | -1 |  | nouveau |
| `0x02FA` | -1 | -1 |  | nouveau |
| `0x02FB` | -1 | -1 |  | nouveau |
| `0x02FC` | -1 | -1 |  | nouveau |
| `0x035C` | 2 | 2 |  |  |
| `0x035D` | -1 | -1 |  |  |
| `0x035E` | 2 | 2 |  |  |
| `0x0439` | 8 | 8 | CZ_UseItem |  |
| `0x043E` | -1 | -1 |  |  |
| `0x0444` | -1 | -1 |  |  |
| `0x0445` | 12 | 10 |  |  |
| `0x0446` | 14 | 14 |  |  |
| `0x0447` | 2 | 2 | CZ_blocking_playcancel |  |
| `0x0448` | -1 | -1 |  |  |
| `0x0449` | 4 | 4 |  |  |
| `0x044A` | 6 | 6 | CZ_CLIENT_VERSION |  |
| `0x07D7` | 8 | 8 | CZ_PartyChangeOption |  |
| `0x07D8` | 8 | 8 |  |  |
| `0x07DA` | 6 | 6 | CZ_PartyChangeLeader |  |
| `0x07E2` | 8 | 8 |  |  |
| `0x07E3` | 6 | 6 |  |  |
| `0x07E5` | 4 | 8 |  |  |
| `0x07E8` | -1 | -1 |  |  |
| `0x07E9` | 5 | 5 |  |  |
| `0x07F5` | 6 | 6 | CZ_GMFullStrip |  |
| `0x07F6` | 14 | 14 |  |  |
| `0x07F7` | -1 | -1 |  |  |
| `0x07F8` | -1 | -1 |  |  |
| `0x07F9` | -1 | -1 |  |  |
| `0x07FC` | 10 | 10 |  |  |
| `0x0803` | 4 | 4 |  |  |
| `0x0805` | -1 | -1 |  |  |
| `0x0807` | 4 | 4 |  |  |
| `0x0809` | 50 | 50 |  |  |
| `0x080A` | 18 | 18 |  |  |
| `0x080B` | 6 | 6 |  |  |
| `0x0810` | 3 | 3 |  |  |
| `0x0812` | 8 | 8 |  |  |
| `0x081A` | 4 | 4 |  |  |
| `0x081B` | 12 | 10 |  |  |
| `0x081C` | 10 | 10 |  |  |
| `0x081D` | 22 | 22 |  |  |
| `0x0820` | 11 | 11 |  |  |
| `0x083B` | 2 | 2 | CZ_CloseSearchStoreInfo |  |
| `0x0842` | 6 | 6 | CZ_GMRecall2 |  |
| `0x0843` | 6 | 6 | CZ_GMRemove2 |  |
| `0x0844` | 2 | 2 | CZ_SE_CASHSHOP_OPEN1 |  |
| `0x0849` | 16 | 16 | ZC_SE_PC_BUY_CASHITEM_RESULT |  |
| `0x084A` | 2 | 2 | CZ_cashshop_close |  |
| `0x084B` | 21 | 19 |  |  |
| `0x0856` | -1 | -1 |  |  |
| `0x0857` | -1 | -1 |  |  |
| `0x0858` | -1 | -1 |  |  |
| `0x08C7` | -1 | 20 |  |  |
| `0x08C9` | 2 | 2 | CZ_cashshop_list_request |  |
| `0x08D2` | 10 | 10 |  |  |
| `0x08D7` | 28 | 28 | CZ_bg_queue_apply_request |  |
| `0x08D8` | 27 | 27 |  |  |
| `0x08D9` | 30 | 30 |  |  |
| `0x08DA` | 26 | 26 | CZ_bg_queue_cancel_request |  |
| `0x08DB` | 27 | 27 |  |  |
| `0x08DC` | 26 | 26 |  |  |
| `0x08DD` | 27 | 27 | CZ_dull |  |
| `0x08DE` | 27 | 27 |  |  |
| `0x08DF` | 50 | 50 |  |  |
| `0x08E0` | 51 | 51 | CZ_bg_queue_lobby_reply |  |
| `0x08E1` | 51 | 51 |  |  |
| `0x08E3` | 157 | 149 |  |  |
| `0x08FE` | -1 | -1 |  |  |
| `0x090A` | 26 | 26 | CZ_bg_queue_request_queue_number |  |
| `0x090F` | -1 | -1 |  |  |
| `0x0914` | -1 | -1 |  |  |
| `0x0915` | -1 | -1 |  |  |
| `0x0974` | 2 | 2 | CZ_merge_item_cancel |  |
| `0x0977` | 14 | 14 |  |  |
| `0x0978` | 6 | 6 | CZ_reqworldinfo |  |
| `0x0979` | 50 | 50 |  |  |
| `0x097A` | -1 | -1 |  |  |
| `0x097F` | -1 | -1 |  |  |
| `0x0980` | 7 | 7 | CZ_SelectCart |  |
| `0x0983` | 29 | 29 |  |  |
| `0x098D` | -1 | -1 | CZ_clan_chat |  |
| `0x099B` | 8 | 8 |  |  |
| `0x099F` | -1 | 22 |  |  |
| `0x09AF` | 4 | 4 |  |  |
| `0x09B1` | 4 | 4 |  |  |
| `0x09B4` | 6 | 6 | CZ_sale_open |  |
| `0x09BC` | 6 | 6 | CZ_sale_close |  |
| `0x09C3` | 10 | 8 | CZ_sale_refresh |  |
| `0x09CA` | -1 | 23 |  |  |
| `0x09CE` | 102 | 102 | CZ_GM_Item_Monster |  |
| `0x09D1` | 14 | 14 |  |  |
| `0x09D4` | 2 | 2 | CZ_NPCShopClosed |  |
| `0x09D7` | -1 | -1 | ZC_NPC_MARKET_PURCHASE_RESULT |  |
| `0x09D8` | 2 | 2 | CZ_NPCMarketClosed |  |
| `0x09DB` | -1 | -1 |  |  |
| `0x09DC` | -1 | -1 |  |  |
| `0x09DD` | -1 | -1 |  |  |
| `0x09E6` | 24 | 22 |  |  |
| `0x09E7` | 3 | 3 |  |  |
| `0x09E8` | 11 | 11 | CZ_Mail_refreshinbox |  |
| `0x09E9` | 2 | 2 | CZ_dull |  |
| `0x09EA` | 11 | 11 | CZ_Mail_read |  |
| `0x09EC` | -1 | -1 | CZ_Mail_send |  |
| `0x09ED` | 3 | 3 |  |  |
| `0x09EE` | 11 | 11 | CZ_Mail_refreshinbox |  |
| `0x09EF` | 11 | 11 | CZ_Mail_refreshinbox |  |
| `0x09F0` | -1 | -1 |  |  |
| `0x09F1` | 11 | 11 | CZ_Mail_getattach |  |
| `0x09F2` | 12 | 12 |  |  |
| `0x09F3` | 11 | 11 | CZ_Mail_getattach |  |
| `0x09F4` | 12 | 12 |  |  |
| `0x09F5` | 11 | 11 | CZ_Mail_delete |  |
| `0x09F6` | 11 | 11 |  |  |
| `0x09F7` | 77 | 75 | ZC_PROPERTY_HOMUN |  |
| `0x09F8` | -1 | -1 |  |  |
| `0x09F9` | 143 | 143 |  |  |
| `0x09FA` | -1 | -1 |  |  |
| `0x09FC` | 6 | 6 |  |  |
| `0x09FD` | -1 | -1 |  |  |
| `0x09FE` | -1 | -1 |  |  |
| `0x09FF` | -1 | -1 |  |  |
| `0x0A00` | 269 | 269 | ZC_SHORTCUT_KEY_LIST |  |
| `0x0A01` | 3 | 3 | CZ_SHORTCUTKEYBAR_ROTATE1 |  |
| `0x0A03` | 2 | 2 | CZ_Mail_cancelwrite |  |
| `0x0A04` | 6 | 6 | CZ_Mail_setattach |  |
| `0x0A06` | 6 | 6 | CZ_Mail_winopen |  |
| `0x0A07` | 9 | 9 |  |  |
| `0x0A08` | 26 | 26 | CZ_Mail_beginwrite |  |
| `0x0A0E` | 14 | 14 | ZC_BATTLEFIELD_NOTIFY_HP |  |
| `0x0A12` | 27 | 27 |  |  |
| `0x0A13` | 26 | 26 | CZ_CHECKNAME1 |  |
| `0x0A14` | 10 | 10 | ZC_CHECKNAME |  |
| `0x0A19` | 2 | 2 | CZ_roulette_open |  |
| `0x0A1A` | 25 | 23 |  |  |
| `0x0A1B` | 2 | 2 | CZ_roulette_info |  |
| `0x0A1C` | -1 | -1 |  |  |
| `0x0A1D` | 2 | 2 | CZ_roulette_close |  |
| `0x0A1E` | 3 | 3 | ZC_ACK_CLOSE_ROULETTE |  |
| `0x0A1F` | 2 | 2 | CZ_roulette_generate |  |
| `0x0A21` | 3 | 3 | CZ_roulette_item |  |
| `0x0A22` | 7 | 5 |  |  |
| `0x0A23` | -1 | -1 |  |  |
| `0x0A24` | 66 | 66 |  |  |
| `0x0A25` | 6 | 6 | CZ_AchievementCheckReward |  |
| `0x0A26` | 7 | 7 |  |  |
| `0x0A2E` | 6 | 6 | CZ_change_title |  |
| `0x0A2F` | 7 | 7 |  |  |
| `0x0A32` | 2 | 2 |  |  |
| `0x0A35` | 4 | 4 | CZ_Oneclick_Itemidentify |  |
| `0x0A44` | -1 | -1 |  |  |
| `0x0A4A` | 6 | 6 |  |  |
| `0x0A4B` | 22 | 22 |  |  |
| `0x0A4C` | 28 | 28 |  |  |
| `0x0A51` | 34 | 34 | ZC_CHECKNAME |  |
| `0x0A68` | 3 | 3 | CZ_open_ui |  |
| `0x0A6E` | -1 | -1 | CZ_Mail_send |  |
| `0x0A77` | 15 | 15 | CZ_VIEW_CAMERAINFO |  |
| `0x0A78` | 15 | 15 | ZC_VIEW_CAMERAINFO |  |
| `0x0A7D` | -1 | -1 |  |  |
| `0x0A97` | 8 | 8 | CZ_equipswitch_add |  |
| `0x0A9A` | 10 | 10 |  |  |
| `0x0A9B` | -1 | -1 |  |  |
| `0x0A9C` | 2 | 2 | CZ_equipswitch_request |  |
| `0x0A9D` | 4 | 4 |  |  |
| `0x0AA5` | -1 | -1 | ZC_MEMBERMGR_INFO |  |
| `0x0AC0` | 26 | 26 | CZ_Mail_refreshinbox |  |
| `0x0AC1` | 26 | 26 | CZ_Mail_refreshinbox |  |
| `0x0ACC` | 18 | 18 |  |  |
| `0x0ACE` | 4 | 4 | CZ_equipswitch_request_single |  |
| `0x0ADD` | 24 | 22 |  |  |
| `0x0ADE` | 6 | 6 |  |  |
| `0x0AE8` | 2 | 2 | CZ_changedress |  |
| `0x0AEF` | 2 | 2 | CZ_attendance_request |  |
| `0x0AF0` | 10 | 10 |  |  |
| `0x0AF4` | 11 | 11 | CZ_UseSkillToPos |  |
| `0x0B4C` | 2 | 2 | CZ_GET_ACCOUNT_LIMTIED_SALE_LIST |  |
| `0x0BFB` | 35 | -1 | CZ_bourgeon_integrity_legacy |  |

## Nouveaux opcodes du build 2026 (60)

| opcode | len | nom connu |
|---|---|---|
| `0x02F8` | -1 | — |
| `0x02F9` | -1 | — |
| `0x02FA` | -1 | — |
| `0x02FB` | -1 | — |
| `0x02FC` | -1 | — |
| `0x0826` | 4 | — |
| `0x0C36` | 12 | — |
| `0x0C37` | -1 | — |
| `0x0C38` | -1 | — |
| `0x0C39` | -1 | — |
| `0x0C3A` | -1 | — |
| `0x0C3B` | 33 | — |
| `0x0C3C` | 6 | — |
| `0x0C3D` | 6 | — |
| `0x0C3E` | 8 | — |
| `0x0C3F` | 7 | — |
| `0x0C40` | 19 | — |
| `0x0C41` | 2 | — |
| `0x0C42` | 19 | — |
| `0x0C43` | 18 | — |
| `0x0C44` | -1 | — |
| `0x0C45` | 2 | — |
| `0x0C46` | -1 | — |
| `0x0C47` | 4 | — |
| `0x0C48` | 46 | — |
| `0x0C49` | 42 | — |
| `0x0C4A` | 2 | — |
| `0x0C4B` | 6 | — |
| `0x0C4C` | 22 | — |
| `0x0C4D` | -1 | — |
| `0x0C4E` | -1 | — |
| `0x0C4F` | 18 | — |
| `0x0C50` | 10 | — |
| `0x0C51` | 10 | — |
| `0x0C52` | -1 | — |
| `0x0C53` | 10 | — |
| `0x0C54` | 2 | — |
| `0x0C55` | 6 | — |
| `0x0C56` | 3 | — |
| `0x0C57` | 7 | — |
| `0x0C58` | 49 | — |
| `0x0C59` | 11 | — |
| `0x0C5A` | 11 | — |
| `0x0C5B` | 30 | — |
| `0x0C5C` | -1 | — |
| `0x0C5E` | 4 | — |
| `0x0C5F` | 4 | — |
| `0x0C60` | 7 | — |
| `0x0C61` | 10 | — |
| `0x0C62` | 34 | — |
| `0x0C63` | 30 | — |
| `0x0C70` | 30 | — |
| `0x0C71` | 8 | — |
| `0x0C72` | 4 | — |
| `0x0C73` | 5 | — |
| `0x0C74` | 10 | — |
| `0x0C75` | 3 | — |
| `0x0C76` | 2 | — |
| `0x0C77` | -1 | — |
| `0x0C79` | 56 | — |

## Disparus depuis 2025 (5)

| opcode | len en 2025 | nom connu | utilisé par Moonlight |
|---|---|---|---|
| `0x0258` | 2 | — | OUI |
| `0x0259` | 3 | — | OUI |
| `0x027E` | -1 | — | OUI |
| `0x02F7` | 47 | ZC_UPDATE_GDID | OUI |
| `0x03DD` | 18 | — | non |

## Le reste du client (non utilisé par Moonlight)

1082 opcodes que le client connaît et que le serveur n'émet pas.
Ils sont dans `packet_len_client_2026.json` (format `opcode: [len, len_bis, flag]`).
