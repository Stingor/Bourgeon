#include "ragnarok/actor.h"

// Le corps assembleur d'`Actor_OnMsg`. Il vit dans un .cc et non dans l'en-tête
// parce qu'un bloc `__asm` dans une fonction `inline` d'un header très inclus
// est exactement le genre de chose qui se compile ici et pas là. Le contrat, la
// forme de la pile et les pièges sont documentés au-dessus de la déclaration,
// dans actor.h — c'est là qu'il faut lire avant d'appeler.

namespace rag::actor {

__declspec(noinline) void SendMsg(void* actor, int msg,
                                  int p1lo, int p1hi,
                                  int p2lo, int p2hi,
                                  int p3lo, int p3hi) {
  void** vtbl = *reinterpret_cast<void***>(actor);
  void* fn = vtbl[2];  // vtable+8 = Actor_OnMsg
  __asm {
    push esi
    mov  esi, esp
    push 0            // paramètres 4 et 5, inutilisés
    push 0
    push 0
    push 0
    mov  eax, p3hi
    push eax
    mov  eax, p3lo
    push eax
    mov  eax, p2hi
    push eax
    mov  eax, p2lo
    push eax
    mov  eax, p1hi
    push eax
    mov  eax, p1lo
    push eax
    push 0            // message, dword de poids fort
    mov  eax, msg
    push eax          // message, dword de poids faible
    push 0            // mot de tête (toujours 0 chez le natif)
    mov  ecx, actor
    mov  eax, fn
    call eax
    mov  esp, esi     // convention de nettoyage inconnue : on restaure nous-mêmes
    pop  esi
  }
}

}  // namespace rag::actor
