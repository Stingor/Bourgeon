#pragma once

// ── Le rect écran d'une fenêtre ImGui, capturé au rendu ──────────────────────
//
// ImGui ne sait où est une fenêtre que PENDANT son rendu. Or les trois viewers
// d'objets — inventaire, chariot, entrepôt — ont besoin de la position des
// AUTRES pour router un glisser : « le joueur a-t-il lâché son objet sur le
// chariot ? » se pose depuis la fenêtre d'inventaire, quand celle du chariot
// n'est plus en cours de dessin.
//
// Le patron est donc : chaque fenêtre note son rect une fois par frame pendant
// son rendu, et les autres l'interrogent quand elles veulent. Rien ici ne
// remplace `viewers::MouseOver*` (features/windows/viewer_probes.h), qui répond
// à la même question pour un point de souris ; c'est la brique en dessous.
//
// 🔴 Pourquoi une classe pour QUATRE FLOTTANTS : le test d'appartenance était
// écrit SIX fois — une fois dans le `PointOverViewer` de chacune des trois
// fenêtres, une fois de plus en expression nue (`over_self`) dans le `OnRenderUI`
// de chacune. Ces six copies étaient à l'octet près, bornes comprises (`>=` en
// haut à gauche, `<` en bas à droite), et rien ne les reliait : les trois
// dernières ne sont même pas des fonctions, donc aucun relevé de doublons par
// corps de fonction ne pouvait les voir.
//
// ⚠ Une différence a été absorbée sciemment : les trois `over_self` ne testaient
// PAS la validité, quand les trois `PointOverViewer` la testaient. Ce n'était
// pas un choix — les `over_self` sont calculés dans le MÊME `OnRenderUI` que la
// capture et APRÈS elle, sans branche entre les deux, donc le rect y est
// toujours valide et le test ajouté ne change rien.
//
// 🔴 `valid()` ne dispense PAS l'appelant de vérifier que la fenêtre est
// ouverte, et c'est pour une raison qui ne saute pas aux yeux : les trois
// viewers rendent dans un ORDRE FIXE, donc l'un interroge toujours le rect que
// l'autre a capturé à la frame PRÉCÉDENTE. Une fenêtre fermée cette frame-ci
// mais pas encore passée dans son propre `OnRenderUI` porte donc un rect valide
// et à jour. Seul son `open_` sait déjà qu'elle est fermée. `Invalidate()` ferme
// le trou dans l'autre sens (une fenêtre qui a cessé d'être dessinée cesse
// d'avoir un rect) mais ne peut rien contre celui-là.
//
// C'est ce trou qui laissait passer un vrai défaut : `ImGui::Begin` rend false
// quand la fenêtre est REPLIÉE ou clippée, et les trois `OnRenderUI` sortaient
// alors avant la capture — le rect gardait sa taille DÉPLIÉE, et un objet lâché
// sur cette surface fantôme partait quand même vers cette fenêtre. Les trois
// sorties invalident désormais.

namespace ro {

class ViewerRect {
 public:
  // À appeler une fois par frame, PENDANT le rendu de la fenêtre.
  void Capture(float x, float y, float w, float h) {
    x_ = x; y_ = y; w_ = w; h_ = h;
    valid_ = true;
  }

  // La fenêtre n'est plus à l'écran : ses coordonnées ne veulent plus rien dire.
  void Invalidate() { valid_ = false; }

  // Le point est-il dedans ? Faux si le rect n'a pas été capturé.
  bool Contains(float mx, float my) const {
    return valid_ && mx >= x_ && my >= y_ && mx < x_ + w_ && my < y_ + h_;
  }

  bool  valid() const { return valid_; }
  float w() const { return w_; }
  float h() const { return h_; }

 private:
  float x_ = 0.0f, y_ = 0.0f, w_ = 0.0f, h_ = 0.0f;
  bool  valid_ = false;
};

}  // namespace ro
