Build de test du fork [oliiivier/OrcaSlicer](https://github.com/oliiivier/OrcaSlicer) (branche `ai-assistant`) :
Snapmaker Orca avec un assistant IA. Non signé, non officiel : ni Snapmaker ni Anthropic ne le distribuent.

## Nouveautés

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
| Windows | `.exe` (installeur) ou `_portable.zip` | SmartScreen : *Informations complémentaires → Exécuter quand même* |
| macOS | `.dmg` | Application non notariée : après l'avoir copiée dans *Applications*, `xattr -dr com.apple.quarantine "/Applications/Snapmaker Orca.app"` |

## À tester sur chaque système

1. Le bouton *Claude* ouvre le chat et active le serveur MCP ; *Aide → AI Assistant* indique le Claude Code
   utilisé (« Used by the chat: … »), surtout pour une installation npm (Windows `claude.cmd`, nvm sur macOS).
2. Une question sur le projet ouvert obtient une réponse qui utilise les outils du slicer.
3. Passage flottant ↔ ancré (bouton de l'en-tête du chat) sans page blanche ni plantage.
4. Captures *3D view* et *Window*.
5. Onglets retrouvés après redémarrage du slicer.

Testé jusqu'ici sous Linux uniquement.
