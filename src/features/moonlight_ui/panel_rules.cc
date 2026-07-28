#include "features/moonlight_ui/internal.h"

#include "imgui.h"
#include "ui/ro_widgets.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

namespace moonlight_ui {

// Charte du serveur : 134 lignes de TEXTE PUR — aucun état, aucun réglage, aucun
// accès à MoonlightUi. C'est précisément pour cela qu'elle sort en premier :
// elle valide la mécanique du découpage sans rien pouvoir casser.
void DrawRules() {
  if (CollapsingHeader("Règles du serveur")) {
    PushStyleCompact();
    RedText("CES RÈGLEMENTS S'APPLIQUENT PARTOUT SUR MOONLIGHT-DESTINY !");
    if (ImGui::TreeNode("Règlements généraux")) {
      TextWrapped("Les règles du serveur doivent être appliquées à la lettre.\nToute personne ne respectant pas la charte sera sanctionnée dans les plus brefs délais.");
      Spacing();
      BulletWrapped("Les joueurs doivent se respecter et garder un langage propre et courtois.");
      BulletWrapped("Les propos visant à rejeter un nouveau joueur sont interdits.");
      BulletWrapped("L'utilisation de programmes tels que bots ou hacks = ban définitif sans hésitation.");
      BulletWrapped("Le flood est strictement interdit.");
      BulletWrapped("Vous êtes entièrement responsable de votre compte.");
      BulletWrapped("Le staff ne rend pas les items perdus (vente NPC, deslotage raté, refine raté).");
      BulletWrapped("Le staff peut exceptionnellement rendre un item perdu si les logs prouvent un bug serveur.");
      BulletWrapped("Ne partagez jamais votre compte ou votre mot de passe.");
      BulletWrapped("La demande de support pour créer un serveur privé est non recommandée.");
      BulletWrapped("Le plagiat volontaire d'un membre du staff est puni.");
      BulletWrapped("Tout ce qui se rapporte au serveur est la propriété exclusive des administrateurs.");
      BulletWrapped("Le langage SMS est à proscrire.");
      BulletWrapped("L'exploitation d'un bug ou abus = sanction. Prévenez immédiatement un administrateur.");
      BulletWrapped("Si vous abusez du cashshop en votant avec plusieurs comptes forum… \ngare à vous c'est comme avec les impôts, \ntant qu'on est pas contrôlé c'est la fête, mais quand ils vous tombent dessus...");
      ImGui::TreePop();
    }
    Spacing();
    if (ImGui::TreeNode("Sur le serveur de jeu")) {
      BulletWrapped("Insultes et vols de drop (Looting) = INTERDITS.");
      BulletWrapped("Heal ou buff un monstre qui ne vous appartient pas sans accord = puni.");
      BulletWrapped("Si vous êtes banni définitivement, tous les comptes liés à votre IP/PC le seront aussi.");
      BulletWrapped("Les sanctions (mute, jail, kick, ban) sont à la discrétion du staff.");
      BulletWrapped("Le Kill Steal est strictement interdit (voir définition). Utilisez @noks pour vous protéger.");
      Spacing();
      BulletWrapped("Les MVPs sont FFA :");
      Indent();
        TextWrapped("Vous pouvez les attaquer même si quelqu'un est dessus.");
        TextWrapped("(À vous de voir si vous voulez passer pour un gros connard selfish en KSant le MVP)");
        TextWrapped("Si vous ne voulez pas vous faire KS, faites @noks <3");
      Unindent();
      ImGui::TreePop();
    }
    Spacing();
    if (ImGui::TreeNode("Le staff")) {
      BulletWrapped("Si vous cassez les couilles du staff ban/delete non temporaire.");
      BulletWrapped("Aucun membre du staff ne vous demandera votre mot de passe.");
      BulletWrapped("Aucun membre du staff ne vous demandera votre login.");
      BulletWrapped("Aucun membre du staff ne vous demandera votre email.");
      BulletWrapped("Seuls les admins peuvent rendre des items perdus suite à un bug serveur.");
      BulletWrapped("Le staff ne rend pas les items prêtés à un joueur disparu/banni.");
      BulletWrapped("Le staff ne donne pas d'items (hors events).");
      BulletWrapped("Les membres du staff ne sont pas des robots. Soyez courtois, cherchez avant de demander.");
      BulletWrapped("Les questions dont la réponse est sur une database = évitez.");
      ImGui::TreePop();
    }
    Spacing();
    if (ImGui::TreeNode("Règlements dans les endroits spécifiques")) {
      if (ImGui::TreeNode("Salle de duel")) {
        BulletWrapped("Ce n'est pas un salon de thé");
        BulletWrapped("Si vous regardez, ok. Sinon, laissez la place.");
        BulletWrapped("Utilisez : @duel, @invite, @accept, @reject, @leave.");
        ImGui::TreePop();
      }
      if (ImGui::TreeNode("Carnage Room")) {
        BulletWrapped("Loi du plus fort.");
        BulletWrapped("Amusez‑vous dans le respect.");
        ImGui::TreePop();
      }
      if (ImGui::TreeNode("PVP Room")) {
        BulletWrapped("Free Kill interdit.");
        ImGui::TreePop();
      }
      if (ImGui::TreeNode("DB Room")) {
        BulletWrapped("Kill Steal STRICTEMENT interdit.");
        BulletWrapped("Si la personne meurt ou se hide les mobs sont à vous.");
        ImGui::TreePop();
      }
      if (ImGui::TreeNode("Guild Dungeon")) {
        BulletWrapped("Libre de tuer les guildiens adverses.");
        ImGui::TreePop();
      }
      if (ImGui::TreeNode("WoE Castles")) {
        BulletWrapped("Interdiction d'apporter de l'aide via un perso non participant (multi-account/perso).");
        BulletWrapped("Les ententes entre guildes sont informelles, non officielles, non sanctionnables.");
        BulletWrapped("Elles doivent être discutées entre guildes dominantes, dans le respect.");
        ImGui::TreePop();
      }
      ImGui::TreePop();
    }
    Spacing();
    if (ImGui::TreeNode("Logiciels tiers")) {
      TextUnformatted("Autorisations :");
      Indent();
        BulletWrapped("Je vais être clair : oui, j'autorise les scripts AHK, les macros clavier/souris, les trucs qui bouclent un sort… tant que ça reste :");
        BulletWrapped("SIMPLE");
        BulletWrapped("BASIQUE");
        BulletWrapped("Pas un tableau de bord de la NASA");
        BulletWrapped("Vous bouclez le spell, éventuellement un clic en plus pour les AOE type Storm Gust, et basta.");
      Unindent();
      TextUnformatted("Quality of Life :");
      Indent();
        BulletWrapped("Le but, c'est du Q.O.L");
        BulletWrapped("Vous préservez votre clavier, votre souris, vos doigts, vos poignets, vos oreilles, et celles de vos voisins qui n'ont rien demandé.");
        BulletWrapped("Bref : du confort, pas du cheat.");
      Unindent();
      TextUnformatted("Les trucs interdits (et je rigole zéro) :");
      Indent();
        RedText("Ne me prenez pas pour un jambon.");
        TextUnformatted("Si vous me sortez :");
        Indent();
          BulletWrapped("un auto-buffer");
          BulletWrapped("un auto-pot");
          BulletWrapped("un super TP/SG de physicien quantique");
          BulletWrapped("un script qui ferait rougir Tony Stark");
        Unindent();
        TextUnformatted("Alors là :");
        Indent();
          BulletWrapped("Je vous fais le fion.");
          BulletWrapped("Je m'en bats les couilles.");
          BulletWrapped("Je vous dégage plus vite que Thanos avec son finger snap. *Snap*");
        Unindent();
        TextUnformatted("Les excuses bidon :");
        Indent();
          BulletWrapped("\"Mais les autres serveurs le font...\"");
          BulletWrapped("\"Mais j'étais pas AFK, je regardais Naruto à côté...\"");
        Unindent();
        TextUnformatted("Résultat :");
        Indent();
          BulletWrapped("Pouf.");
          BulletWrapped("Vous étiez sur Moon.");
          BulletWrapped("Vous ne l'êtes plus.");
          BulletWrapped("Et il ne restera de vous que des ruines numériques sur Wayback Machine.");
        Unindent();
      Unindent();
      ImGui::TreePop();
    }
    PopStyleCompact();
  }
}

}  // namespace moonlight_ui
