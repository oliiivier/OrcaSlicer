Build de test du fork [oliiivier/OrcaSlicer](https://github.com/oliiivier/OrcaSlicer) (branche `ai-assistant`) :
Snapmaker Orca avec un assistant IA. Non signé, non officiel : ni Snapmaker ni Anthropic ne le distribuent.

## Nouveautés de la v2.4.1-ai.2

- **L'assistant modifie les objets** comme l'onglet *Préparer*, après une seule confirmation et en une étape
  d'annulation : redimensionner, pivoter, déplacer, centrer, miroir, couper, séparer, dupliquer, supprimer,
  orienter, disposer, ajouter une pièce, une pièce négative, un modificateur ou un bloqueur de supports,
  changer le filament d'un objet ou d'une pièce, renommer.
- **Instructions et notes** (fenêtre *AI Assistant*) : des instructions que vous écrivez, lues au début de
  chaque conversation, et un dossier de notes Markdown que l'assistant consulte et complète (préférences,
  particularités de l'imprimante et des filaments, résultats de calibration). Écriture désactivable.
- **Documentation Snapmaker en direct** : l'assistant cherche et lit le wiki (wiki.snapmaker.com) et le site
  de support (support.snapmaker.com) pour les recommandations du constructeur, en citant la page et sa date.
  Désactivable (les recherches partent chez Snapmaker).
- **Choix du modèle Claude** sous le champ de saisie du chat.

## Déjà dans la v2.4.1-ai.1

- **Chat avec Claude dans le slicer** (bouton *Claude* de la barre du haut) : flottant ou ancré à droite de la
  vue 3D, une conversation par onglet (conservées après redémarrage), capture de la vue 3D ou de la fenêtre en
  un clic. Nécessite [Claude Code](https://claude.com/claude-code) installé, avec une connexion faite une fois
  dans un terminal (`claude`).
- **Serveur MCP** (*Aide → AI Assistant (MCP server)*) : Claude, Gemini ou un autre client MCP lit le projet,
  explique les avertissements, propose des réglages (chacun confirmé dans le slicer), suit l'imprimante
  connectée et adapte un projet prévu pour une autre imprimante.

## Installation

| Système | Fichier | Remarque |
|---|---|---|
| Linux | `.AppImage` | `chmod +x` puis lancer |
| Arch Linux, CachyOS | `.pkg.tar.zst` | `sudo pacman -U <fichier>` ; lancer `snapmaker-orca` |
| Windows | `.exe` (installeur) ou `_portable.zip` | SmartScreen : *Informations complémentaires → Exécuter quand même* |

Pas de version macOS pour l'instant.

## À tester sur chaque système

1. Demander à l'assistant de couper ou redimensionner un objet : confirmation, résultat, puis Ctrl+Z.
2. Écrire une instruction dans *Aide → AI Assistant*, ouvrir une nouvelle conversation et vérifier qu'elle est
   suivie ; demander à l'assistant de noter quelque chose, puis *Ouvrir* le dossier des notes.
3. Une question de dépannage (« des fils sur ma pièce en PETG ») : l'assistant cite une page du wiki Snapmaker.
4. Le bouton *Claude* ouvre le chat et active le serveur MCP ; *Aide → AI Assistant* indique le Claude Code
   utilisé (« Used by the chat: … »), surtout pour une installation npm (Windows `claude.cmd`).
5. Une question sur le projet ouvert obtient une réponse qui utilise les outils du slicer.
6. Passage flottant ↔ ancré (bouton de l'en-tête du chat) sans page blanche ni plantage.
7. Captures *3D view* et *Window*.
8. Onglets retrouvés après redémarrage du slicer.

Testé jusqu'ici sous Linux uniquement.
