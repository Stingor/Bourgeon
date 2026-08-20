#pragma once

#include <string>

const std::string kYamlConfiguration = R"(
# 2015-11-04aRagexe
20151102:
  CSession:
    layout: 20151102
    CSession: 0x0093DA20
    GetTalkType: 0x0094AFB0
  UIWindowMgr:
    UIWindowMgr: 0x0060C820
    ProcessPushButton: 0x00613100
    SendMsg: 0x00623E20
  CRagConnection:
    CConnection: 0x00814BB0
    SendPacket: 0x00815630
  CModeMgr:
    Switch: 0x0065AE30
  CLoginMode:
    OnUpdate: 0x0090CE90
  CGameMode:
    OnUpdate: 0x0088D730
    ProcessInput: 0x0088e6d0

# 2017-06-14bRagexeRE
20170613:
  CSession:
    layout: 20170613
    CSession: 0x00a50b70
    GetTalkType: 0x00a5e960
  UIWindowMgr:
    UIWindowMgr: 0x006a2dd0
    ProcessPushButton: 0x006aa6e0
    SendMsg: 0x006bcfc0
  CRagConnection:
    CConnection: 0x0091d470
    SendPacket: 0x0091e1f0
  CModeMgr:
    Switch: 0x006f4800
  CLoginMode:
    OnUpdate: 0x00A1F2A0
  CGameMode:
    OnUpdate: 0x00996910
    ProcessInput: 0x00997b50

# 2019-01-16cRagexe
20190116:
  CSession:
    layout: 20190116
    CSession: 0x00a145b0
    GetTalkType: 0x00a226f0
  UIWindowMgr:
    UIWindowMgr: 0x007166b0
    ProcessPushButton: 0x0071df50
    SendMsg: 0x00731350
  CRagConnection:
    CConnection: 0x008d4580
    SendPacket: 0x008d5410
  CModeMgr:
    Switch: 0x0075f850
  CLoginMode:
    OnUpdate: 0x009DED20
  CGameMode:
    OnUpdate: 0x0094EE20
    ProcessInput: 0x009500f0

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
)";