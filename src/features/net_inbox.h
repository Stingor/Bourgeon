#pragma once

// ── PacketInbox : le passage du fil RÉSEAU au fil PRINCIPAL ──────────────────
//
// 🔴 LE PROBLÈME, ET IL EST GÉNÉRAL. `Plugin::OnRecvPacket` est appelé sur le fil
// RÉSEAU ; `OnTick` et `OnRenderUI` sur le fil PRINCIPAL. Décoder un paquet
// directement dans ses membres, c'est écrire pendant que le rendu lit :
//
//   - un `push_back` qui RÉALLOUE laisse au rendu un pointeur mort en plein
//     parcours — le crash ne se reproduit qu'en jeu, jamais en test ;
//   - un `clear()` en pleine boucle d'affichage, pareil ;
//   - une `std::map`/`unordered_map` qui REHASHE pendant un `find`, pareil ;
//   - une `std::string` réassignée pendant qu'ImGui en lit le `c_str()`, pareil.
//
// Aucun de ces bugs n'est visible à la lecture du plugin fautif : le code a l'air
// séquentiel. C'est le genre de défaut qu'on ne trouve qu'en se demandant « qui
// appelle cette fonction, et depuis quel fil ». D'où cette brique, et d'où le fait
// qu'elle soit la MÊME partout.
//
// ── LE PATRON ────────────────────────────────────────────────────────────────
//
// Le fil réseau ne décode RIEN : il COPIE les octets et rend la main. Le décodage
// entier — celui qui existait déjà, inchangé — repart au tick, sur le fil
// principal. Trois lignes par module :
//
//   void X::OnRecvPacket(uint16_t op, const uint8_t* data, uint16_t len) {
//     net_inbox_.Push(op, data, len);          // fil RÉSEAU : rien d'autre
//   }
//   void X::OnTick() {
//     net_inbox_.Drain([this](uint16_t op, const uint8_t* d, uint16_t n) {
//       HandlePacket(op, d, n);                // l'ANCIEN corps, tel quel
//     });
//     ...
//   }
//
// Copier plutôt que décoder est le point important : le tampon `data` appartient
// au lecteur de paquets du client et ne survit pas au retour de OnRecvPacket. Il
// n'y a donc pas de version « on garde le pointeur ».
//
// ⚠ CE QUI DOIT RESTER SUR LE FIL RÉSEAU. Un handler natif REMPLACÉ tournait sur
// ce fil-là : ce qu'il écrivait tout de suite doit continuer de l'être, sinon on
// laisse une fenêtre d'une frame où le client se croit dans un autre état (p. ex.
// `CGameMode+0x24C`, l'interaction NPC, qui autorise le déplacement tant qu'il
// n'est pas posé). Ces écritures-là sont des `mov` d'entier sur une globale,
// atomiques sur x86 et sans allocation : les garder AVANT le Push est correct, et
// c'est le seul cas qui le soit. Tout ce qui touche à un conteneur, à une chaîne,
// à une fenêtre ou au natif passe par la file.
//
// ⚠ CE QUE ÇA NE COUVRE PAS. La file sérialise les DONNÉES, pas les décisions :
// un plugin qui décidait quelque chose au recv le décide maintenant un tick plus
// tard. C'est presque toujours ce qu'on veut (le rendu voit un état cohérent),
// mais ça change l'ordre relatif avec un handler NATIF encore vivant sur un
// opcode OBSERVÉ — à vérifier quand les deux se parlent.

#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace bourgeon {

class PacketInbox {
 public:
  // ── Fil RÉSEAU ──────────────────────────────────────────────────────────────
  // Copie `len` octets de `data`. C'est la forme normale : `len` est ce que le
  // dispatcher a promis de transmettre.
  void Push(uint16_t opcode, const uint8_t* data, uint16_t len) {
    PushBytes(opcode, data, len);
  }

  // Paquet à LONGUEUR ANNONCÉE (`data` commence sur le champ longueur, comme
  // partout dans ce projet : listes de boutique, de fabrication, de cash shop…).
  //
  // 🔴 Ces handlers-là lisent AU-DELÀ de `len`. Ce n'est pas un abus : le
  // dispatcher ne transmet que les premiers octets, le reste vit dans le tampon
  // de réception, complet, et les modules s'en servent depuis toujours. Copier
  // `len` octets casserait donc ces listes en silence. On lit la longueur
  // annoncée (elle inclut l'opcode, d'où le -2) et on copie tout le corps.
  //
  // Repli sur `len` si la longueur annoncée est absurde : un paquet corrompu ne
  // doit pas nous faire lire à l'aveugle dans le tampon du client.
  void PushAnnounced(uint16_t opcode, const uint8_t* data, uint16_t len) {
    uint16_t size = len;
    if (data && len >= 2) {
      const uint16_t announced = *reinterpret_cast<const uint16_t*>(data);
      if (announced >= 4 && announced <= kMaxPacket)
        size = static_cast<uint16_t>(announced - 2);  // -2 : l'opcode est déjà consommé
      if (size < len) size = len;  // jamais MOINS que ce qui est promis
    }
    PushBytes(opcode, data, size);
  }

  // ── Fil PRINCIPAL ───────────────────────────────────────────────────────────
  // Rejoue les paquets dans l'ORDRE D'ARRIVÉE : `fn(opcode, data, len)`.
  // `data` pointe dans un tampon qui vit le temps du Drain — le handler décode et
  // n'en garde rien, exactement comme il le faisait du tampon du client.
  //
  // Le verrou n'est PAS tenu pendant `fn` : un handler peut envoyer un paquet,
  // ouvrir une fenêtre, et le fil réseau peut continuer d'empiler.
  template <typename Fn>
  void Drain(Fn&& fn) {
    slots_out_.clear();
    bytes_out_.clear();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (slots_.empty()) return;
      // Les tampons de sortie, vidés juste au-dessus, repartent en file : leur
      // capacité est réutilisée, la file redevient vide, et on n'alloue plus rien
      // après les premiers paquets.
      slots_.swap(slots_out_);
      bytes_.swap(bytes_out_);
    }
    for (const Slot& s : slots_out_)
      fn(s.opcode, bytes_out_.data() + s.offset, s.size);
  }

  // Jette ce qui reste : la session à laquelle ces paquets appartenaient est
  // abandonnée (bascule d'interrupteur, changement de map, déconnexion).
  void Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    slots_.clear();
    bytes_.clear();
  }

  // Paquets refusés faute de place depuis le début (diagnostic). Non nul = le fil
  // principal ne draine plus, ou un déluge inattendu sur un opcode.
  uint32_t dropped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
  }

 private:
  // Plafonds. La file est vidée à chaque tick : elle ne peut grossir que si le fil
  // principal s'est arrêté (chargement de map, freeze) — auquel cas jeter vaut
  // mieux que gonfler indéfiniment. Large de deux ordres de grandeur par rapport à
  // ce qu'une frame voit passer, même sous AoE.
  static constexpr uint16_t kMaxPacket = 0x7FFF;
  static constexpr size_t   kMaxSlots  = 2048;
  static constexpr size_t   kMaxBytes  = 512 * 1024;

  struct Slot {
    uint16_t opcode;
    uint16_t size;
    uint32_t offset;
  };

  void PushBytes(uint16_t opcode, const uint8_t* data, uint16_t size) {
    if (!data) size = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (slots_.size() >= kMaxSlots || bytes_.size() + size > kMaxBytes) {
      ++dropped_;
      return;
    }
    Slot s;
    s.opcode = opcode;
    s.size   = size;
    s.offset = static_cast<uint32_t>(bytes_.size());
    if (size) bytes_.insert(bytes_.end(), data, data + size);
    slots_.push_back(s);
  }

  // File vivante (sous verrou) et tampons de sortie (fil principal seul).
  std::vector<Slot>    slots_;
  std::vector<uint8_t> bytes_;
  std::vector<Slot>    slots_out_;
  std::vector<uint8_t> bytes_out_;
  mutable std::mutex   mutex_;
  uint32_t             dropped_ = 0;
};

}  // namespace bourgeon
