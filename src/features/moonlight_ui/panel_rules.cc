#include "features/moonlight_ui/internal.h"

#include "imgui.h"
#include "ui/ro_widgets.h"
#include "utils/i18n.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

namespace moonlight_ui {

// Charte du serveur : 134 lignes de TEXTE PUR — aucun état, aucun réglage, aucun
// accès à MoonlightUi. C'est précisément pour cela qu'elle sort en premier :
// elle valide la mécanique du découpage sans rien pouvoir casser.
void DrawRules() {
  if (CollapsingHeader(i18n::Tr("Règles du serveur"))) {
    PushStyleCompact();
    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "%s", i18n::Tr("CES RÈGLEMENTS S'APPLIQUENT PARTOUT SUR MOONLIGHT-DESTINY !"));
    if (ImGui::TreeNode(i18n::Tr("Règlements généraux"))) {
      TextWrapped(i18n::Tr("Les règles du serveur doivent être appliquées à la lettre.\nToute personne ne respectant pas la charte sera sanctionnée dans les plus brefs délais."));
      Spacing();
      BulletWrapped(i18n::Tr("Les joueurs doivent se respecter et garder un langage propre et courtois."));
      BulletWrapped(i18n::Tr("Les propos visant à rejeter un nouveau joueur sont interdits."));
      BulletWrapped(i18n::Tr("L'utilisation de programmes tels que bots ou hacks = ban définitif sans hésitation."));
      BulletWrapped(i18n::Tr("Le flood est strictement interdit."));
      BulletWrapped(i18n::Tr("Vous êtes entièrement responsable de votre compte."));
      BulletWrapped(i18n::Tr("Le staff ne rend pas les items perdus (vente NPC, deslotage raté, refine raté)."));
      BulletWrapped(i18n::Tr("Le staff peut exceptionnellement rendre un item perdu si les logs prouvent un bug serveur."));
      BulletWrapped(i18n::Tr("Ne partagez jamais votre compte ou votre mot de passe."));
      BulletWrapped(i18n::Tr("La demande de support pour créer un serveur privé est non recommandée."));
      BulletWrapped(i18n::Tr("Le plagiat volontaire d'un membre du staff est puni."));
      BulletWrapped(i18n::Tr("Tout ce qui se rapporte au serveur est la propriété exclusive des administrateurs."));
      BulletWrapped(i18n::Tr("Le langage SMS est à proscrire."));
      BulletWrapped(i18n::Tr("L'exploitation d'un bug ou abus = sanction. Prévenez immédiatement un administrateur."));
      BulletWrapped(i18n::Tr("Si vous abusez du cashshop en votant avec plusieurs comptes forum… \ngare à vous c'est comme avec les impôts, \ntant qu'on est pas contrôlé c'est la fête, mais quand ils vous tombent dessus..."));
      ImGui::TreePop();
    }
    Spacing();
    if (ImGui::TreeNode(i18n::Tr("Sur le serveur de jeu"))) {
      BulletWrapped(i18n::Tr("Insultes et vols de drop (Looting) = INTERDITS."));
      BulletWrapped(i18n::Tr("Heal ou buff un monstre qui ne vous appartient pas sans accord = puni."));
      BulletWrapped(i18n::Tr("Si vous êtes banni définitivement, tous les comptes liés à votre IP/PC le seront aussi."));
      BulletWrapped(i18n::Tr("Les sanctions (mute, jail, kick, ban) sont à la discrétion du staff."));
      BulletWrapped(i18n::Tr("Le Kill Steal est strictement interdit (voir définition). Utilisez @noks pour vous protéger."));
      Spacing();
      BulletWrapped(i18n::Tr("Les MVPs sont FFA :"));
      Indent();
        TextWrapped(i18n::Tr("Vous pouvez les attaquer même si quelqu'un est dessus."));
        TextWrapped(i18n::Tr("(À vous de voir si vous voulez passer pour un gros connard selfish en KSant le MVP)"));
        TextWrapped(i18n::Tr("Si vous ne voulez pas vous faire KS, faites @noks <3"));
      Unindent();
      ImGui::TreePop();
    }
    Spacing();
    if (ImGui::TreeNode(i18n::Tr("Le staff"))) {
      BulletWrapped(i18n::Tr("Si vous cassez les couilles du staff ban/delete non temporaire."));
      BulletWrapped(i18n::Tr("Aucun membre du staff ne vous demandera votre mot de passe."));
      BulletWrapped(i18n::Tr("Aucun membre du staff ne vous demandera votre login."));
      BulletWrapped(i18n::Tr("Aucun membre du staff ne vous demandera votre email."));
      BulletWrapped(i18n::Tr("Seuls les admins peuvent rendre des items perdus suite à un bug serveur."));
      BulletWrapped(i18n::Tr("Le staff ne rend pas les items prêtés à un joueur disparu/banni."));
      BulletWrapped(i18n::Tr("Le staff ne donne pas d'items (hors events)."));
      BulletWrapped(i18n::Tr("Les membres du staff ne sont pas des robots. Soyez courtois, cherchez avant de demander."));
      BulletWrapped(i18n::Tr("Les questions dont la réponse est sur une database = évitez."));
      ImGui::TreePop();
    }
    Spacing();
    if (ImGui::TreeNode(i18n::Tr("Règlements dans les endroits spécifiques"))) {
      if (ImGui::TreeNode(i18n::Tr("Salle de duel"))) {
        BulletWrapped(i18n::Tr("Ce n'est pas un salon de thé"));
        BulletWrapped(i18n::Tr("Si vous regardez, ok. Sinon, laissez la place."));
        BulletWrapped("Utilisez : @duel, @invite, @accept, @reject, @leave.");
        ImGui::TreePop();
      }
      if (ImGui::TreeNode(i18n::Tr("Carnage Room"))) {
        BulletWrapped(i18n::Tr("Loi du plus fort."));
        BulletWrapped(i18n::Tr("Amusez‑vous dans le respect."));
        ImGui::TreePop();
      }
      if (ImGui::TreeNode(i18n::Tr("PVP Room"))) {
        BulletWrapped(i18n::Tr("Free Kill interdit."));
        ImGui::TreePop();
      }
      if (ImGui::TreeNode(i18n::Tr("DB Room"))) {
        BulletWrapped(i18n::Tr("Kill Steal STRICTEMENT interdit."));
        BulletWrapped(i18n::Tr("Si la personne meurt ou se hide les mobs sont à vous."));
        ImGui::TreePop();
      }
      if (ImGui::TreeNode(i18n::Tr("Guild Dungeon"))) {
        BulletWrapped(i18n::Tr("Libre de tuer les guildiens adverses."));
        ImGui::TreePop();
      }
      if (ImGui::TreeNode(i18n::Tr("WoE Castles"))) {
        BulletWrapped(i18n::Tr("Interdiction d'apporter de l'aide via un perso non participant (multi-account/perso)."));
        BulletWrapped(i18n::Tr("Les ententes entre guildes sont informelles, non officielles, non sanctionnables."));
        BulletWrapped(i18n::Tr("Elles doivent être discutées entre guildes dominantes, dans le respect."));
        ImGui::TreePop();
      }
      ImGui::TreePop();
    }
    Spacing();
    if (ImGui::TreeNode(i18n::Tr("Logiciels tiers"))) {
      TextUnformatted("Autorisations :");
      Indent();
        BulletWrapped(i18n::Tr("Je vais être clair : oui, j'autorise les scripts AHK, les macros clavier/souris, les trucs qui bouclent un sort… tant que ça reste :"));
        BulletWrapped("SIMPLE");
        BulletWrapped("BASIQUE");
        BulletWrapped(i18n::Tr("Pas un tableau de bord de la NASA"));
        BulletWrapped(i18n::Tr("Vous bouclez le spell, éventuellement un clic en plus pour les AOE type Storm Gust, et basta."));
      Unindent();
      TextUnformatted(i18n::Tr("Quality of Life :"));
      Indent();
        BulletWrapped(i18n::Tr("Le but, c'est du Q.O.L"));
        BulletWrapped(i18n::Tr("Vous préservez votre clavier, votre souris, vos doigts, vos poignets, vos oreilles, et celles de vos voisins qui n'ont rien demandé."));
        BulletWrapped(i18n::Tr("Bref : du confort, pas du cheat."));
      Unindent();
      TextUnformatted(i18n::Tr("Les trucs interdits (et je rigole zéro) :"));
      Indent();
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "%s", i18n::Tr("Ne me prenez pas pour un jambon."));
        TextUnformatted(i18n::Tr("Si vous me sortez :"));
        Indent();
          BulletWrapped(i18n::Tr("un auto-buffer"));
          BulletWrapped(i18n::Tr("un auto-pot"));
          BulletWrapped(i18n::Tr("un super TP/SG de physicien quantique"));
          BulletWrapped(i18n::Tr("un script qui ferait rougir Tony Stark"));
        Unindent();
        TextUnformatted(i18n::Tr("Alors là :"));
        Indent();
          BulletWrapped(i18n::Tr("Je vous fais le fion."));
          BulletWrapped(i18n::Tr("Je m'en bats les couilles."));
          BulletWrapped(i18n::Tr("Je vous dégage plus vite que Thanos avec son finger snap. *Snap*"));
        Unindent();
        TextUnformatted(i18n::Tr("Les excuses bidon :"));
        Indent();
          BulletWrapped(i18n::Tr("\"Mais les autres serveurs le font...\""));
          BulletWrapped(i18n::Tr("\"Mais j'étais pas AFK, je regardais Naruto à côté...\""));
        Unindent();
        TextUnformatted(i18n::Tr("Résultat :"));
        Indent();
          BulletWrapped("Pouf.");
          BulletWrapped(i18n::Tr("Vous étiez sur Moon."));
          BulletWrapped(i18n::Tr("Vous ne l'êtes plus."));
          BulletWrapped(i18n::Tr("Et il ne restera de vous que des ruines numériques sur Wayback Machine."));
        Unindent();
      Unindent();
      ImGui::TreePop();
    }
    PopStyleCompact();
  }
}

}  // namespace moonlight_ui
