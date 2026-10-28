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
   - `resources/web/ai_assistant/` (page du chat) et `resources/images/topbar_claude.svg`
   - `scripts/fork/`
2. Les fichiers upstream ne sont touchés qu'aux **points d'accroche** ci-dessous, toujours entourés de
   `#ifdef SNAPMAKER_AI_ASSISTANT // fork`. Si un nouveau point est nécessaire, l'ajouter à cette liste.
3. Toujours intégrer upstream par **merge** (jamais de rebase de `ai-assistant`, jamais de squash des fusions upstream).

### Points d'accroche dans le code upstream

| Fichier                         | Modification                                                  |
|---------------------------------|---------------------------------------------------------------|
| `src/slic3r/CMakeLists.txt`     | `include(AIAssistant/AIAssistant.cmake OPTIONAL)` après `encoding_check(libslic3r_gui)` |
| `src/slic3r/GUI/MainFrame.cpp`  | include de `AIAssistant.hpp` + appel `append_menu_items()` à la fin de `generate_help_menu()` |
| `src/slic3r/GUI/BBLTopbar.cpp`  | include de `AIAssistant.hpp` + `add_topbar_button()` après *Calibration* dans `Init()`, `rescale_topbar_button()` dans `Rescale()` |
| `src/slic3r/GUI/GLCanvas3D.cpp` | include de `ScreenCapture.hpp` + `ScreenCapture::on_canvas_frame()` juste avant `SwapBuffers()` dans `render()` |
| `src/slic3r/GUI/GUI_ObjectList.cpp` | Linux : couleurs du slicer pour la liste des objets (le thème GTK peut être clair quand le slicer est sombre) |

Lister les écarts réels avec upstream (doit rester court) :

```bash
git diff upstream/main...ai-assistant --stat -- . ':!src/slic3r/AIAssistant' ':!resources/ai_assistant' \
    ':!resources/web/ai_assistant' ':!resources/images/topbar_claude.svg' ':!scripts/fork' ':!FORK.md'
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

Menu *Aide → AI Assistant (MCP server)* : activation, port (13700 par défaut), jeton, chemin de Claude Code pour
le chat, et configuration à copier dans Claude Code, Gemini CLI ou Claude Desktop (inutile pour le chat intégré,
qui se connecte seul). Le serveur écoute uniquement sur `127.0.0.1`, exige
`Authorization: Bearer <jeton>` et refuse les requêtes venant d'un navigateur (en-têtes `Origin`/`Host`).

| Fichier | Rôle |
|---|---|
| `McpServer` | Transport Streamable HTTP + JSON-RPC, indépendant de l'interface (testable seul) |
| `McpTools` | Registre des outils et prompts, exécution sur le thread de l'interface |
| `McpIssues` | Collecte des erreurs et avertissements du slicer (`get_issues`) |
| `McpChanges` | Validation, confirmation et application des réglages (`apply_changes`) |
| `McpChangesDialog` | Fenêtre de confirmation : une case par réglage, aide du réglage et commentaire du modèle au survol |
| `McpMaterials` | Têtes, emplacements de filament, filaments mixtes, attribution par élément |
| `McpSlicing` | Slicing du plateau et résumé du résultat (`slice`) |
| `McpRender` | Rendu PNG du plateau depuis une vue nommée (`render_view`) |
| `McpPrinter` | Lecture seule de l'imprimante connectée (`get_printer_status`, écarts projet/imprimante) |
| `McpPrinterLive` | Lecture en direct (`printer.objects.query`), photo de la caméra (`printer_snapshot`), suivi d'impression (`watch_print`) |
| `McpPrinterControl` | Seule commande envoyée à l'imprimante : déchargement des têtes (`unload_filament`) |
| `PrinterAutoConnect` | Connexion automatique au démarrage quand une seule imprimante est en ligne |
| `ProjectAdapt` | Adaptation d'un projet à l'imprimante connectée (`adapt_to_printer`) et notification |
| `McpExperiences` | Journal local des retours d'expérience (`record_experience`, `report_outcome`, `search_experiences`) |
| `McpDocs` | Documentation des réglages livrée avec l'application (`describe_setting`, `read_documentation`) |
| `McpService` | Cycle de vie du serveur (démarrage, arrêt à la fermeture) |
| `McpSettings` | Section `[ai_assistant]` de la configuration de l'application |
| `McpDialog` | Fenêtre de réglages |

Outils : `get_issues`, `get_project`, `get_config`, `describe_setting`, `read_documentation`, `apply_changes`,
`slice`, `render_view`, `adapt_to_printer`, `get_printer_status`, `printer_snapshot`, `watch_print`, `unload_filament`,
`record_experience`, `report_outcome`, `search_experiences`.

Prompts : `adapt_project`, `diagnose_print`, `fix_issues`, `optimize_settings`, `capitalize_experiences`. Dans
Claude Code ils s'appellent `/mcp__snapmaker-orca__<prompt>` (taper `/` puis le nom) et ne sont chargés qu'au
démarrage : relancer Claude Code si le slicer n'était pas ouvert à ce moment.

Connexion automatique (case dans la fenêtre MCP, activée par défaut) : au démarrage, pendant 2 minutes, si
aucune imprimante n'est connectée et qu'une seule est en ligne (`query.device.list`), le slicer s'y connecte
(`action.device.connect`, comme l'onglet Appareil). Une seule tentative ; jamais si plusieurs sont en ligne.

Adaptation d'un projet prévu pour une autre imprimante (3MF MakerWorld…) : outil `adapt_to_printer` et
prompt `adapt_project`, plus une notification dans le slicer quand le projet ouvert vise un autre modèle que
l'imprimante connectée (case dans la fenêtre MCP, activée par défaut ; surveillance par minuterie, sans point
d'accroche). Après confirmation : preset de l'imprimante connectée avec sa buse, procédé à la hauteur de couche
la plus proche, et chaque emplacement de filament déplacé vers la tête chargée la plus proche (même matériau,
puis couleur la plus proche). Sur la U1 l'emplacement N imprime par défaut avec la tête N : le projet prend un
emplacement par tête et tout suit (peinture, filament des objets, volumes et plages, filaments par fonction).
Les réglages de procédé d'origine qui diffèrent sont renvoyés pour que l'assistant restaure l'intention du
modèle.

`apply_changes` montre chaque réglage proposé avec une case cochée, l'ancienne et la nouvelle valeur et le
commentaire du modèle (`comment`) ; le survol affiche aussi l'aide du réglage. L'utilisateur applique tout ou
décoche des valeurs : celles-ci reviennent au modèle sous `declined` et sont notées dans le journal
d'expériences (`declined_by_user`), pour le suivi de l'impression.

`apply_changes` refuse `post_process` (scripts exécutés sur l'ordinateur à l'export du G-code), même confirmé.

Accès à l'imprimante, via la connexion Snapmaker Connect déjà ouverte par l'application ; deux cases à cocher
dans la fenêtre MCP, désactivées par défaut :

- **Lecture** : état (`query.device.objects`, ou `printer.objects.query` pour ce qui change vite), photo de
  la caméra (URL de capture de la webcam déclarée à Moonraker, `server.webcams.list`, en général
  `http://<ip>/webcam/snapshot.jpg` sur le réseau local ; pas `camera.start_monitor`, dont le flux reste
  actif ~6 min après l'arrêt), suivi borné d'une impression. Numéros de série, réseau, comptes et secrets sont
  retirés avant envoi au modèle.
- **Contrôle** (exige la lecture) : uniquement `unload_filament`, refusé pendant une impression, confirmé
  dans le slicer à chaque demande. Même séquence que l'écran de la U1 : `AUTO_FEEDING_BATCH ACTION=START`,
  puis `ACTION=DOING STAGE=prepare EXTRUDER=n UNLOAD=1` (le firmware chauffe lui-même). L'écran de
  l'imprimante envoie ensuite `STAGE=doing` dès `unload_heat_finish` (l'outil ne l'envoie qu'après 15 s sans
  relais), et `ACTION=STOP` termine (chauffes coupées), y compris si l'appel est abandonné. Le firmware
  s'arrête à `unload_finish` : le filament est sorti de la buse mais reste dans la tête, l'utilisateur le
  retire à la main. Les filaments souples (`filament_soft`) ne sont jamais déchargés.

La documentation vient de `doc/` (wiki OrcaSlicer) : `scripts/fork/build-setting-docs.py` relie chaque réglage
à sa section grâce aux liens d'aide de `Tab.cpp` et produit `resources/ai_assistant/setting_docs.json`, livré
avec l'application (aucun accès réseau). `sync-upstream.sh` le régénère et le commite après chaque fusion.
Prompts (commandes slash dans Claude Code / Gemini CLI) : `adapt_project`, `diagnose_print`, `fix_issues`,
`optimize_settings`, `capitalize_experiences`.

### Retours d'expérience

1. **Journal local** (`<dossier de config>/ai_assistant/experiences/*.json`, hors dépôt) : une fiche par
   diagnostic. Le serveur y met les faits (imprimante, matériaux, problèmes, réglages modifiés ou refusés via
   `apply_changes`), le modèle le symptôme et le diagnostic, puis le résultat réel de l'impression
   (`report_outcome`, avec le rapport de l'imprimante si l'accès est activé).
2. **Capitalisation** : prompt `capitalize_experiences` lancé dans Claude Code depuis ce dépôt. Il regroupe
   les fiches confirmées et rédige `resources/ai_assistant/guides/exp_*.md` (format et règles :
   `resources/ai_assistant/guides/README.md`), puis attend la relecture du diff.
3. **Injection** : après relecture, commit. Les guides sont livrés dans `setting_docs.json` et servis par
   `read_documentation` (type `experience`).

`render_view` montre le modèle (pas les supports ni les trajectoires) ; pour l'aperçu tranché, l'utilisateur
peut coller une capture d'écran dans le client. Le serveur traite une requête à la fois : pendant un `slice`
(jusqu'à 10 min), un `watch_print` ou un `unload_filament`, les autres requêtes attendent.

Multi-têtes (Snapmaker U1, 4 têtes) : `get_project.materials` décrit les têtes et chaque emplacement
(preset, type, couleur, température, tête, utilisé sur le plateau) ; les réglages par emplacement/tête sont
renvoyés en tableaux ; `apply_changes` cible un emplacement (`filament_slot`) ou une tête (`toolhead`).
Un seul preset de filament peut avoir des modifications non enregistrées à la fois : l'assistant doit
faire enregistrer le précédent avant de passer à un autre emplacement.

Les instructions envoyées au client imposent l'ordre : erreurs bloquantes, puis avertissements, puis
optimisations. Toute modification passe par une confirmation dans le slicer ; les surcharges par objet
s'annulent avec Ctrl+Z, les modifications de presets restent des éditions non enregistrées.

## Chat Claude

Bouton *Claude* de la barre du haut (ou *Aide → Chat with Claude*) : un chat avec Claude dans le slicer,
flottant ou ancré à droite de la vue 3D (bouton dans l'en-tête du chat, choix mémorisé). Il fait tourner la CLI
Claude Code installée sur la machine (`claude -p` en stream-json, connexion faite une fois dans un terminal) et
la relie au serveur MCP, qu'il active s'il est éteint. La CLI n'a aucun autre outil (`--tools=`, pas de shell,
de fichiers ni de web) et tourne dans un dossier vide (`ai_assistant/chat`). `claude` est cherché dans le
PATH, les emplacements d'installation habituels puis le PATH du shell de connexion (nvm, préfixe npm…) ; sous
Windows, le raccourci `claude.cmd` de npm est lu pour lancer sa cible sans `cmd.exe`. Chemin imposable dans la
fenêtre *AI Assistant* (`claude_path`).

Onglets : une conversation (session Claude Code, reprise par `--resume`) par onglet, enregistrées avec ce
qu'elles affichent dans `ai_assistant/chats.json`. La CLI d'un onglet inactif et sans réponse en cours est
arrêtée, puis relancée à son message suivant. Boutons de capture de la vue 3D (relue dans OpenGL) ou de toute la
fenêtre, jointe au message suivant. La configuration MCP donnée à la CLI (avec le jeton) est écrite dans
`ai_assistant/claude_mcp.json`, lisible par l'utilisateur seul. La page du chat est
`resources/web/ai_assistant/chat.html`.

Testé sous Linux uniquement : la recherche de `claude` sous Windows et macOS, le passage de la page entre
fenêtre et volet, et la capture de la fenêtre entière (copie d'écran) restent à vérifier sur ces systèmes.

| Fichier | Rôle |
|---|---|
| `ClaudeProcess` | Lance la CLI, écrit et lit les messages JSON ligne par ligne, interruption |
| `ChatPanel` | Page du chat, onglets et sessions, fenêtre flottante ou volet wxAUI du `Plater` |
| `ScreenCapture` | Capture de la vue 3D et de la fenêtre principale |

## Compilation

L'assistant est activé par défaut. Pour compiler sans lui (et vérifier que le fork reste neutre) :

```bash
cmake -DSNAPMAKER_AI_ASSISTANT=OFF ...
```
