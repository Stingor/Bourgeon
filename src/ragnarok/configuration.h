#pragma once

#include <string>

const std::string kYamlConfiguration = R"(
# 🔴 UNE SEULE VERSION DE CLIENT EST RÉELLEMENT SUPPORTÉE.
#
# Trois autres ont figuré ici — 2015-11-04a, 2017-06-14b, 2019-01-16c — avec
# treize adresses chacune. C'était une promesse que le projet ne tenait pas :
# le reste de src/ appelle 407 adresses EN DUR (et en cite 475 autres dans ses
# blocs de RE), toutes propres au 20250716. Ces trois blocs n'avaient d'ailleurs
# aucun des champs de réception (RecvDispatchTable, PacketLenLookup,
# RecvDispatchLoopHead) : même la couche réseau y tournait en mode dégradé. Un
# client plus ancien démarrait, puis tombait au premier hook.
#
# ⚠ Rouvrir une version, ce n'est donc PAS ajouter un bloc ici : c'est relever
# les 400 autres. Retirés le 2026-08-26 (git les conserve).

# 2025-07-16_Ragexe
# Confirmed: all addresses confirmed via Ghidra analysis.
# Confirmed: hp_/sp_/aid_ session offsets confirmed via Ghidra WRITE/READ xref analysis.
# Confirmed: ProcessPushButton 0x00a471e0 = FUN_00a471e0, called from WndProc for WM_KEYDOWN/WM_SYSKEYDOWN.
# Note: ProcessInput 0x00c86740 is CMode::SendMsg(msg, p1, p2, p3, p4) — vtable
# slot +0x18 of CGameMode vtable 0x010904b8 (ctor FUN_00c63570). Unlike older
# clients' no-arg ProcessInput, it takes 5 stack args with callee cleanup
# (RET 0x14), hence ProcessInputArgs below.
20250716:
  CSession:
    layout: 20250716
    CSession: 0x00d57780
    GetTalkType: 0x00D5E590
  UIWindowMgr:
    UIWindowMgr: 0x00a29ba0
    ProcessPushButton: 0x00a471e0
    SendMsg: 0x00a4ad20
  CRagConnection:
    CConnection: 0x00c13fc0
    SendPacket: 0x00c14920
    RecvDispatchTable: 0x00caa2e0
    RecvOpcodeBase: 0x73
    RecvDispatchTableSize: 0xBC3
    # Tete de la boucle de depilage de la fonction de reception (0x00c9df00) :
    # le point ou convergent tous les `case` natifs pour lire le paquet suivant.
    # Notre stub y reboucle au lieu de quitter la fonction -- sans quoi la
    # reception est plafonnee a UN paquet revendique par frame (la fonction
    # n'est appelee qu'une fois par frame), soit ~7 ms par paquet.
    RecvDispatchLoopHead: 0x00c9e1dd
    RecvOpcodeReader: 0x00c144b0
    RecvBufferReset: 0x00c148b0
    # Resolveur de longueur du client : PacketLenTable_Lookup(table, out[2], opcode)
    # remplit out[0] = 1 (longueur FIXE, out[1] = octets, opcode compris) ou -1/0
    # (VARIABLE : la longueur se lit dans les deux octets qui suivent l'opcode).
    # C'est la table que la boucle recv consulte elle-meme (RecvBuffer_ReadPacket
    # 0x00c147d0) : s'en servir evite de coder une longueur en dur par paquet.
    PacketLenLookup: 0x00aa7b00
    PacketLenTable: 0x0159d68c
  CModeMgr:
    Switch: 0x00a756e0
  CLoginMode:
    OnUpdate: 0x00d272e0
  CGameMode:
    OnUpdate: 0x00c74a80
    ProcessInput: 0x00c86740
    ProcessInputArgs: 5
    # GameMode_PostActorClickAction : ce que le clic sur une entite ARME sur
    # l'acteur du joueur (message 10 -> action en attente +0x500, cible +0x514).
    # C'est de cet armement que decoulent l'approche ET le coup ; la selection,
    # elle, est deja ecrite par l'appelant. Optionnel : absent, le reglage
    # « le clic cible sans attaquer » n'empeche que le coup.
    PostActorClickAction: 0x00c753a0
  CScene:
    RenderCellsAndCursor: 0x00a7b0a0

# Client 2026-07-07. Les 19 adresses ont ete retrouvees le 2026-08-27, chacune
# verifiee par un critere INDEPENDANT de la methode qui l'a trouvee -- appelants,
# slot de vtable, ratio de taille, convention d'appel. Detail et preuves dans
# docs/2026/boot_addresses.md.
#
# 🔴 DEUX RESERVES CONNUES, a lever avant de compter sur ce client :
#
#   - SendMsg (UIWindowMgr_ChatAction) a gagne UN ARGUMENT : `retn 18h` en 2026
#     contre `retn 14h` en 2025. La paire est bonne (meme fonction, meme nom,
#     ratio de taille 1,03, 316 appelants) -- c'est l'API du client qui a change.
#     Le hook `UIWindowMgr::SendMsgHook` prend 5 arguments et depilera 20 octets
#     la ou le natif en depile 24 : la pile serait corrompue au premier appel.
#     A adapter AVANT d'activer ce client.
#
#   - le layout de CSession est celui du 2025 : c'est le seul implemente, et il
#     n'a PAS ete valide sur ce build. Un offset de CGameMode a deja bouge
#     (0x40C -> 0x3E4 dans PostActorClickAction), donc il faut s'attendre a ce
#     que celui de CSession ait bouge aussi. Le demarrage ne le lit pas ; les
#     lectures de session, si.
#
20260707:
  CSession:
    layout: 20250716
    CSession: 0x00c6a730
    GetTalkType: 0x00c71310
  UIWindowMgr:
    # ctor : ecrit ??_7UIWindowMgr@@6B@ dans [edi], comme en 2025. Deux fonctions
    # l'ecrivent des deux cotes ; on prend la plus grosse, et les deux ratios de
    # taille concordent (1648/1871 = 0,88 et 1006/1157 = 0,87).
    UIWindowMgr: 0x009f8ed0
    # nom deja pose par un portage anterieur, VERIFIE ici : son appelant
    # sub_CC7240 contient DefWindowProcA (c'est la WndProc) et fait exactement
    # 3 appels, comme Game_MainWndProc en 2025.
    ProcessPushButton: 0x00a15160
    SendMsg: 0x00a18a20
  CRagConnection:
    CConnection: 0x00bdeca0
    # C'est la fonction d'emission qui ajoute l'octet de controle en queue de
    # paquet (cf. docs/2026/protocol_entry_2026.md) : 5071 octets contre 95 en
    # 2025, tout le code de hachage etant dedans. `retn 8` des deux cotes.
    SendPacket: 0x00bdf440
    # Table de sauts du switch principal, lue par get_switch_info :
    # jumps = 0x0051610C, ncases = 3029 (0xBD5), lowcase = 115 (0x73).
    RecvDispatchTable: 0x0051610c
    RecvOpcodeBase: 0x73
    RecvDispatchTableSize: 0xBD5
    # Tete de la boucle de depilage : cible convergente des `case`. Le code y est
    # identique a celui de 2025, instruction par instruction (cmp <replay>, 0 /
    # call GetInstance / push <buffer> / call RecvBuffer_ReadPacket).
    RecvDispatchLoopHead: 0x005096f9
    # 13 et 44 octets, signature d'octets exacte, et surtout LES MEMES DEUX
    # appelants qu'en 2025 (la boucle map et la boucle login/char).
    RecvOpcodeReader: 0x00bdee70
    RecvBufferReset: 0x00bdf3d0
    PacketLenLookup: 0x00aa4290
    PacketLenTable: 0x0146edfc
  CModeMgr:
    # Meme sequence d'appels virtuels qu'en 2025 (vt+8, vt+4, vt+0Ch), appelee
    # seulement depuis WinMain avec "login.rsw", `retn 8` des deux cotes.
    Switch: 0x00a3c8c0
  CLoginMode:
    # Slot #4 de ??_7CLoginMode@@6B@. Temoin d'alignement des vtables : le slot
    # #7 est la plus grosse fonction des DEUX cotes (10 017 o en 2025 ; en 2026
    # c'est celle qui construit CZ_ENTER).
    OnUpdate: 0x00c3a300
  CGameMode:
    OnUpdate: 0x004cef40
    # `retn 14h` = 5 arguments, ce qui confirme ProcessInputArgs ci-dessous.
    ProcessInput: 0x004e37a0
    ProcessInputArgs: 5
    # Retrouvee par ses reperes propres -- push 133h avec 2710h ET 9C40h a
    # portee : UN seul site dans toute l'image. Structure identique a 2025.
    PostActorClickAction: 0x004cfad0
  CScene:
    # Slot #3 de ??_7CView@@6B@. La table s'appelle « g_CCamera_vtable » cote
    # 2025, mais son RTTI dit ??_R4CView@@6B@ : c'est CView, pas CCamera.
    RenderCellsAndCursor: 0x00a41a00
)";