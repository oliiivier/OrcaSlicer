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
   - `resources/ai_assistant/` (documentation générée pour le serveur MCP)
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
git diff upstream/main...ai-assistant --stat -- . ':!src/slic3r/AIAssistant' ':!resources/ai_assistant' ':!scripts/fork' ':!FORK.md'
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
| `McpTools` | Registre des outils et prompts, exécution sur le thread de l'interface |
| `McpIssues` | Collecte des erreurs et avertissements du slicer (`get_issues`) |
| `McpChanges` | Validation, confirmation et application des réglages (`apply_changes`) |
| `McpMaterials` | Têtes, emplacements de filament, filaments mixtes, attribution par élément |
| `McpSlicing` | Slicing du plateau et résumé du résultat (`slice`) |
| `McpRender` | Rendu PNG du plateau depuis une vue nommée (`render_view`) |
| `McpPrinter` | Lecture seule de l'imprimante connectée (`get_printer_status`, écarts projet/imprimante) |
| `McpExperiences` | Journal local des retours d'expérience (`record_experience`, `report_outcome`, `search_experiences`) |
| `McpDocs` | Documentation des réglages livrée avec l'application (`describe_setting`, `read_documentation`) |
| `McpService` | Cycle de vie du serveur (démarrage, arrêt à la fermeture) |
| `McpSettings` | Section `[ai_assistant]` de la configuration de l'application |
| `McpDialog` | Fenêtre de réglages |

Outils : `get_issues`, `get_project`, `get_config`, `describe_setting`, `read_documentation`, `apply_changes`,
`slice`, `render_view`, `get_printer_status`, `record_experience`, `report_outcome`, `search_experiences`.

Accès à l'imprimante : désactivé par défaut (case à cocher dans la fenêtre MCP), en lecture seule, via la
connexion Snapmaker Connect déjà ouverte par l'application (`query.device.objects`). Numéros de série, réseau,
comptes et secrets sont retirés avant envoi au modèle. Aucune commande n'est jamais envoyée à l'imprimante.

La documentation vient de `doc/` (wiki OrcaSlicer) : `scripts/fork/build-setting-docs.py` relie chaque réglage
à sa section grâce aux liens d'aide de `Tab.cpp` et produit `resources/ai_assistant/setting_docs.json`, livré
avec l'application (aucun accès réseau). `sync-upstream.sh` le régénère et le commite après chaque fusion.
Prompts (commandes slash dans Claude Code / Gemini CLI) : `diagnose_print`, `fix_issues`, `optimize_settings`,
`capitalize_experiences`.

### Retours d'expérience

1. **Journal local** (`<dossier de config>/ai_assistant/experiences/*.json`, hors dépôt) : une fiche par
   diagnostic. Le serveur y met les faits (imprimante, matériaux, problèmes, réglages modifiés par
   `apply_changes`), le modèle le symptôme et le diagnostic, puis le résultat réel de l'impression
   (`report_outcome`, avec le rapport de l'imprimante si l'accès est activé).
2. **Capitalisation** : prompt `capitalize_experiences` lancé dans Claude Code depuis ce dépôt. Il regroupe
   les fiches confirmées et rédige `resources/ai_assistant/guides/exp_*.md` (format et règles :
   `resources/ai_assistant/guides/README.md`), puis attend la relecture du diff.
3. **Injection** : après relecture, commit. Les guides sont livrés dans `setting_docs.json` et servis par
   `read_documentation` (type `experience`).

`render_view` montre le modèle (pas les supports ni les trajectoires) ; pour l'aperçu tranché, l'utilisateur
peut coller une capture d'écran dans le client. Le serveur traite une requête à la fois : pendant un `slice`
(jusqu'à 10 min), les autres requêtes attendent.

Multi-têtes (Snapmaker U1, 4 têtes) : `get_project.materials` décrit les têtes et chaque emplacement
(preset, type, couleur, température, tête, utilisé sur le plateau) ; les réglages par emplacement/tête sont
renvoyés en tableaux ; `apply_changes` cible un emplacement (`filament_slot`) ou une tête (`toolhead`).
Un seul preset de filament peut avoir des modifications non enregistrées à la fois : l'assistant doit
faire enregistrer le précédent avant de passer à un autre emplacement.

Les instructions envoyées au client imposent l'ordre : erreurs bloquantes, puis avertissements, puis
optimisations. Toute modification passe par une confirmation dans le slicer ; les surcharges par objet
s'annulent avec Ctrl+Z, les modifications de presets restent des éditions non enregistrées.

## Compilation

L'assistant est activé par défaut. Pour compiler sans lui (et vérifier que le fork reste neutre) :

```bash
cmake -DSNAPMAKER_AI_ASSISTANT=OFF ...
```
