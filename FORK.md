# Fork oliiivier/OrcaSlicer — Assistant IA

Fork de [Snapmaker/OrcaSlicer](https://github.com/Snapmaker/OrcaSlicer) qui intègre un serveur
[MCP](https://modelcontextprotocol.io) : un assistant (Claude, Gemini…) peut analyser le projet et la configuration,
recommander des réglages et, à terme, les appliquer après confirmation dans le slicer.
Ce fork n'a pas vocation à être fusionné en amont : l'objectif est de pouvoir intégrer les mises à jour upstream
avec un minimum de conflits.

## Branches

| Branche              | Rôle                                                                  |
|----------------------|-----------------------------------------------------------------------|
| `main`               | Miroir exact de `upstream/main`. **Aucun commit du fork dessus.**     |
| `ai-assistant`       | Branche par défaut du fork : upstream + fonctionnalités du fork.      |
| `feat/*`             | Développement des fonctionnalités, PR vers `ai-assistant`.            |

## Règles pour limiter les conflits

1. Tout le code du fork vit dans des dossiers qui n'existent pas upstream :
   - `src/slic3r/AIAssistant/` (C++ et `AIAssistant.cmake`)
   - `scripts/fork/`
2. Les fichiers upstream ne sont touchés qu'aux **points d'accroche** ci-dessous, toujours entourés de
   `#ifdef SNAPMAKER_AI_ASSISTANT // fork`. Si un nouveau point est nécessaire, l'ajouter à cette liste.
3. Toujours intégrer upstream par **merge** (jamais de rebase de `ai-assistant`, jamais de squash des fusions upstream).

### Points d'accroche dans le code upstream

| Fichier                         | Modification                                                  |
|---------------------------------|---------------------------------------------------------------|
| `src/slic3r/CMakeLists.txt`     | `include(AIAssistant/AIAssistant.cmake OPTIONAL)` après `encoding_check(libslic3r_gui)` |
| `src/slic3r/GUI/MainFrame.cpp`  | include de `AIAssistant.hpp` + appel `append_menu_items()` à la fin de `generate_help_menu()` |

Lister les écarts réels avec upstream (doit rester court) :

```bash
git diff upstream/main...ai-assistant --stat -- . ':!src/slic3r/AIAssistant' ':!scripts/fork' ':!FORK.md'
```

## Synchronisation avec upstream

GitHub Actions est **désactivé** sur ce fork (réglage du dépôt, pas une modification de fichier) :
les workflows hérités d'upstream (`.github/workflows/`) restent dans l'arbre, intacts, mais ne tournent pas,
y compris ceux qu'upstream ajoutera plus tard. La synchronisation et la compilation se font en local :

```bash
scripts/fork/sync-upstream.sh        # fusionne upstream/main dans ai-assistant et remet main sur upstream/main
scripts/fork/sync-upstream.sh -b     # idem puis compile (dépendances déjà compilées)
scripts/fork/sync-upstream.sh -b -p  # idem puis pousse main et ai-assistant sur origin
```

En cas de conflit, le script s'arrête en listant les fichiers concernés (normalement seulement les points
d'accroche) : les résoudre, `git commit`, puis relancer le script.

Réactiver Actions si besoin : `gh api -X PUT repos/oliiivier/OrcaSlicer/actions/permissions -F enabled=true`.

## Serveur MCP

Menu *Aide → AI Assistant (MCP server)* : activation, port (13700 par défaut), jeton, et configuration à copier
dans Claude Code, Gemini CLI ou Claude Desktop. Le serveur écoute uniquement sur `127.0.0.1`, exige
`Authorization: Bearer <jeton>` et refuse les requêtes venant d'un navigateur (en-têtes `Origin`/`Host`).

| Fichier | Rôle |
|---|---|
| `McpServer` | Transport Streamable HTTP + JSON-RPC, indépendant de l'interface (testable seul) |
| `McpTools` | Outils exposés, exécutés sur le thread de l'interface |
| `McpService` | Cycle de vie du serveur (démarrage, arrêt à la fermeture) |
| `McpSettings` | Section `[ai_assistant]` de la configuration de l'application |
| `McpDialog` | Fenêtre de réglages |

Outils disponibles : `get_project`, `get_config`, `describe_setting` (lecture seule).

## Compilation

L'assistant est activé par défaut. Pour compiler sans lui (et vérifier que le fork reste neutre) :

```bash
cmake -DSNAPMAKER_AI_ASSISTANT=OFF ...
```
