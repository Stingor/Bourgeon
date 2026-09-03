# Diff octet a octet entre l'exe VANILLA (celui de l'IDB) et l'exe LIVRE (patche par WARP).
# Rend la carte des patchs, groupes en regions contigues, avec leur RVA.
# L'IDB etant l'exe vanilla, ces regions sont INVISIBLES dans IDA.
import struct, os, json, sys

VANILLA = r"E:\Nouveau dossier\2025-07-16_Ragexe_175220998_clientinfo.exe"
LIVRE   = r"E:\Nouveau dossier\Moonlight-Destiny\Moonlight-Destiny.exe"
IMGBASE = 0x400000
TROU_MAX = 16   # deux differences separees de <= 16 octets = une seule region

def sections(data):
    e = struct.unpack_from('<I', data, 0x3C)[0]
    assert data[e:e+4] == b'PE\0\0'
    n = struct.unpack_from('<H', data, e + 6)[0]
    opt = struct.unpack_from('<H', data, e + 20)[0]
    tbl = e + 24 + opt
    out = []
    for i in range(n):
        o = tbl + i * 40
        name = data[o:o+8].rstrip(b'\0').decode('latin1')
        vsize, vaddr, rsize, raddr = struct.unpack_from('<IIII', data, o + 8)
        out.append((name, vaddr, vsize, raddr, rsize))
    return out

def off_to_va(secs, off):
    for name, va, vs, ra, rs in secs:
        if ra <= off < ra + rs:
            return IMGBASE + va + (off - ra), name
    return None, None

for p in (VANILLA, LIVRE):
    if not os.path.exists(p):
        sys.exit("ABSENT : " + p)

a = open(VANILLA, 'rb').read()
b = open(LIVRE, 'rb').read()
print("vanilla %d o | livre %d o" % (len(a), len(b)))
if len(a) != len(b):
    print("!! tailles differentes — WARP a change la taille, la comparaison par offset reste valable sur le prefixe commun")

secs = sections(a)
n = min(len(a), len(b))

# 1) positions differentes
diffs = [i for i in range(n) if a[i] != b[i]]
print("octets differents : %d" % len(diffs))

# 2) regroupement en regions
regions = []
if diffs:
    debut = prev = diffs[0]
    for i in diffs[1:]:
        if i - prev > TROU_MAX:
            regions.append((debut, prev))
            debut = i
        prev = i
    regions.append((debut, prev))

print("regions de patch : %d" % len(regions))
print()

lignes = []
par_section = {}
for (d, f) in regions:
    va, sec = off_to_va(secs, d)
    par_section[sec] = par_section.get(sec, 0) + 1
    lignes.append({
        "off": d,
        "va": va,
        "section": sec,
        "taille": f - d + 1,
        "vanilla": a[d:f+1][:24].hex(),
        "livre": b[d:f+1][:24].hex(),
    })

print("repartition par section :", par_section)
print()
print("%-10s %-10s %-8s %-6s" % ("offset", "VA", "section", "octets"))
print("-" * 46)
for L in lignes:
    print("0x%08X 0x%08X %-8s %-6d" % (L["off"], L["va"] or 0, L["section"] or "?", L["taille"]))

# 3) zones qui nous interessent aujourd hui
ZONES = {
    "UIMenuIconWnd_BuildIconList": (0x812FB0, 0x8138A0),
    "table de visibilite icones":  (0x814064, 0x814144),
    "UIMenuIcon_SetHelpTextByCmdId": (0x814550, 0x814A70),
    "UIMenuIconWnd_OnMsg":         (0x814A70, 0x814F93),
}
print()
print("=== ces regions touchent-elles les fonctions du menu d icones ? ===")
for nom, (v0, v1) in ZONES.items():
    dedans = [L for L in lignes if L["va"] and v0 <= L["va"] < v1]
    if dedans:
        print("  %-32s : %d region(s) PATCHEE(S)" % (nom, len(dedans)))
        for L in dedans:
            print("        VA 0x%08X (%d o) vanilla=%s livre=%s" % (L["va"], L["taille"], L["vanilla"], L["livre"]))
    else:
        print("  %-32s : INTACTE" % nom)

with open("warp_patch_map.json", "w", encoding="utf-8", newline="") as f:
    json.dump(lignes, f, indent=1)
print()
print("carte complete ecrite dans warp_patch_map.json (%d regions)" % len(lignes))
