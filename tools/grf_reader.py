# -*- coding: utf-8 -*-
"""Lecteur GRF v2.0 en lecture seule, sans dépendance.

Sert aux outils Python du projet à lire les ressources du client SANS exiger
qu'elles soient extraites sur disque. Reproduit la résolution du client :
**disque d'abord, GRF ensuite** — le VFS du jeu fait pareil.

⚠ `data.grf` de Moonlight est CHIFFRÉ (signature « Event Horizon ») : ce lecteur
le refuse explicitement plutôt que de rendre des octets faux. `moonlight.grf` et
`palettes.grf` sont des GRF standard et se lisent normalement.

Les noms sont stockés en CP949 ; on les indexe en minuscules ASCII, comme le
`_strlwr` de `Grf_NormalizePath` côté client.
"""

import os
import struct
import zlib

SIGNATURE = b"Master of Magic"
ENTETE = 46
META = 17          # compressedSize+aligned+realSize+flags+offset


def _minuscule_ascii(octets):
    """Abaisse SEULEMENT A-Z : un second octet CP949 peut tomber dans cette
    plage, et le toucher casserait le nom coréen."""
    return bytes(c + 32 if 0x41 <= c <= 0x5A else c for c in octets)


class Grf(object):
    def __init__(self, chemin):
        self.chemin = chemin
        with open(chemin, "rb") as f:
            entete = f.read(ENTETE)
            if entete[:15] != SIGNATURE:
                raise ValueError(
                    "%s : signature %r — GRF chiffré ou format inconnu, "
                    "illisible sans la clé" % (chemin, entete[:15]))
            offset, seed, count, version = struct.unpack_from("<IIII", entete, 30)
            if version >> 8 != 2:
                raise ValueError("%s : version 0x%X non gérée" % (chemin, version))
            f.seek(ENTETE + offset)
            clen, _ulen = struct.unpack("<II", f.read(8))
            brut = zlib.decompress(f.read(clen))

        self.entrees = {}
        i, restants = 0, count - seed - 7
        for _ in range(restants):
            j = brut.index(b"\x00", i)
            nom = brut[i:j]
            i = j + 1
            csize, _aligned, rsize, flags, doff = struct.unpack_from("<IIIBI", brut, i)
            i += META
            if flags & 0x01:            # bit 0 = fichier (sinon répertoire)
                self.entrees[_minuscule_ascii(nom)] = (doff, csize, rsize, flags)

    def __contains__(self, nom):
        return _minuscule_ascii(self._octets(nom)) in self.entrees

    def __len__(self):
        return len(self.entrees)

    @staticmethod
    def _octets(nom):
        return nom if isinstance(nom, bytes) else nom.encode("cp949")

    def noms(self):
        """Noms bruts (octets CP949, minuscules ASCII)."""
        return self.entrees.keys()

    def lire(self, nom):
        cle = _minuscule_ascii(self._octets(nom))
        entree = self.entrees.get(cle)
        if entree is None:
            return None
        doff, csize, rsize, flags = entree
        if flags & 0x06:                # DES : réservé aux GRF protégés
            raise ValueError("%r est chiffré (flags 0x%02X)" % (nom, flags))
        with open(self.chemin, "rb") as f:
            f.seek(ENTETE + doff)
            donnees = f.read(csize)
        return zlib.decompress(donnees)[:rsize]


class Vfs(object):
    """Disque d'abord, puis les GRF dans l'ordre donné — comme le client."""

    def __init__(self, racine, grfs=()):
        self.racine = racine
        self.grfs = []
        for nom in grfs:
            chemin = os.path.join(racine, nom)
            if not os.path.isfile(chemin):
                continue
            try:
                self.grfs.append(Grf(chemin))
            except ValueError:
                pass                    # chiffré ou exotique : on l'ignore

    def _variantes_disque(self, chemin_relatif):
        """Les DEUX orthographes possibles d'un chemin coréen sur disque.

        Extrait par un outil moderne, un dossier s'appelle `인간족` ; extrait
        par le client (ou par un outil qui recrache les octets bruts) il
        s'appelle `Àΰ£Á·` — les mêmes octets CP949 relus en CP1252. Un client
        fr-FR ne voit QUE la seconde forme, mais nos outils Python peuvent
        tomber sur l'une ou l'autre. Cf. reference_data_folder_cp949_encoding.
        """
        yield chemin_relatif
        try:
            yield chemin_relatif.encode("cp949").decode("cp1252")
        except (UnicodeEncodeError, UnicodeDecodeError):
            pass

    def lire(self, chemin_relatif):
        """`chemin_relatif` commence à `data\\` (comme dans le GRF)."""
        for variante in self._variantes_disque(chemin_relatif):
            sur_disque = os.path.join(self.racine, variante)
            if os.path.isfile(sur_disque):
                with open(sur_disque, "rb") as f:
                    return f.read()
        for grf in self.grfs:
            donnees = grf.lire(chemin_relatif)
            if donnees is not None:
                return donnees
        return None

    def lister(self, prefixe, suffixe=None):
        """Noms (str CP949-décodés) commençant par `prefixe`. Disque + GRF."""
        pref = _minuscule_ascii(
            prefixe if isinstance(prefixe, bytes) else prefixe.encode("cp949"))
        suf = None
        if suffixe:
            suf = _minuscule_ascii(
                suffixe if isinstance(suffixe, bytes) else suffixe.encode("cp949"))

        trouves = set()
        for grf in self.grfs:
            for nom in grf.noms():
                if nom.startswith(pref) and (suf is None or nom.endswith(suf)):
                    trouves.add(nom)

        for variante in self._variantes_disque(prefixe):
            dossier = os.path.join(self.racine, variante.replace("/", os.sep))
            if not os.path.isdir(dossier):
                continue
            for fichier in os.listdir(dossier):
                try:                     # un nom lu en CP1252 se réencode en
                    octets = fichier.encode("cp1252")   # ses octets CP949...
                except UnicodeEncodeError:
                    octets = fichier.encode("cp949", "replace")   # ...sinon il
                brut = _minuscule_ascii(octets)          # était déjà en coréen
                if suf is None or brut.endswith(suf):
                    trouves.add(pref.rstrip(b"\\") + b"\\" + brut)

        return sorted(n.decode("cp949", "replace") for n in trouves)
