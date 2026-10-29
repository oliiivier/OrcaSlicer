# Plan : suite du serveur MCP

Ce plan couvre les fonctions d'Orca encore hors de portée du serveur (liste de tâches de `FORK.md`) et
la remise en ordre des outils existants. Il sert de référence pour les prochaines étapes ; on le met à jour
à chaque phase terminée.

Décisions prises :

- Outils regroupés **par domaine**, avec un paramètre `action`.
- Mode test qui accepte les confirmations **compilé uniquement dans les builds de debug**.
- Périmètre : préréglages et filaments, couches et aperçu, annulation et calibrations, texte, SVG et
  peinture automatique.
- Écartés : chauffe, déplacement des axes et mise à l'origine de l'imprimante, peinture manuelle,
  préférences de l'application, compte Snapmaker et MakerWorld.

## 1. Pourquoi revoir le découpage

Le socle tient et reste tel quel : exécution sur le fil de l'interface, tâches longues suivies par appels
répétés (`ToolRetry`) avec abandon propre, confirmation de chaque modification, réutilisation des dialogues
et commandes du slicer (`Plater`, événements de la barre d'outils, instantanés d'annulation).

Trois défauts grandiraient avec 12 à 15 outils de plus :

1. **Trop d'outils.** Il y en a 30. Chaque description part au modèle à chaque conversation, et le choix du
   bon outil se dégrade quand la liste s'allonge.
2. **Conventions divergentes.** Les résultats disent `applied` ici et `done` là, les messages de refus et
   de délai dépassé sont recopiés dans chaque fichier, et trois styles de confirmation coexistent.
3. **Instructions monolithiques.** Le texte envoyé à l'initialisation (`INSTRUCTIONS` dans `McpTools.cpp`)
   grossit à chaque ajout.

S'y ajoute un manque : rien ne teste automatiquement les parcours avec confirmation.

## 2. Liste cible des outils

Règles de nommage : `get_*` pour les lectures, `edit_*` pour ce qui modifie le projet, `control_printer`
pour ce qui agit sur l'imprimante. Un outil d'écriture prend une liste d'opérations (`operations`) quand
enchaîner plusieurs actions a un sens, sinon une seule `action`.

| Outil cible | Actions | Remplace ou étend |
|---|---|---|
| `get_issues` | | inchangé |
| `get_project` | | inchangé ; ajoute les préréglages disponibles et les événements par couche |
| `get_config` | | inchangé |
| `get_docs` | `setting`, `page` | `describe_setting`, `read_documentation` |
| `get_view` | `model`, `preview` | `render_view` ; ajoute l'aperçu tranché |
| `get_printer` | `status`, `snapshot`, `watch` | `get_printer_status`, `printer_snapshot`, `watch_print` |
| `notes` | `list`, `read`, `write` | `list_notes`, `read_note`, `write_note` |
| `experiences` | `search`, `record`, `report` | `search_experiences`, `record_experience`, `report_outcome` |
| `manufacturer_docs` | `search`, `read` | `search_manufacturer_docs`, `read_manufacturer_page` |
| `apply_changes` | | inchangé (réglages, surcharges par objet) |
| `edit_presets` | `select`, `save`, `add_slot`, `remove_slot`, `set_color` | nouveau |
| `edit_objects` | opérations actuelles + `add_height_range`, `remove_height_range`, `add_text`, `add_svg`, `paint_supports`, `clear_paint` | étend |
| `edit_plates` | opérations actuelles + `add_layer_event`, `remove_layer_event` | étend |
| `edit_project` | `new`, `open`, `import`, `save`, `export` | `new_project`, `open_project`, `import_models`, `save_project`, `export_file` |
| `adapt_to_printer` | | inchangé |
| `slice` | | inchangé (`all_plates`) |
| `calibrate` | `temperature`, `flow`, `pressure_advance`, `retraction`, `max_volumetric_speed`, `vfa` | nouveau |
| `undo` | `undo`, `redo` | nouveau |
| `control_printer` | `start_print`, `pause`, `resume`, `cancel`, `unload_filament`, `load_filament` | `start_print`, `control_print`, `unload_filament` |

Cible : **19 outils** au lieu des 30 actuels, toutes les fonctions prévues comprises.

Le délai d'un outil est fixé par outil (`Tool::timeout`, `retry_for`) : un outil fusionné prend le plus long
de ses actions. `get_printer` (`watch` attend jusqu'à 10 min) et `control_printer` (`unload_filament` jusqu'à
30 min) le permettent sans changement du moteur ; l'abandon (`Tool::abandon`) dispatche selon l'action
enregistrée dans l'état.

Compatibilité : les clients relisent la liste des outils à chaque connexion. Les notes et les guides qui
citent d'anciens noms sont mis à jour ; les instructions de l'utilisateur ne sont pas réécrites, mais un
avertissement est ajouté dans `FORK.md`.

## 3. Conventions communes

Une base commune dans `McpToolUtils` (ou un nouveau `McpActions`), utilisée par tous les outils d'écriture.

- **Confirmation.** `confirm(reason, lines, question)` : liste des opérations, question, Oui/Non, un seul
  style. Les réglages gardent leur fenêtre dédiée (`McpChangesDialog`). Quand le slicer a son propre dialogue
  (fichier, impression), il sert de confirmation.
- **Résultat.** Toujours `done` (booléen). Refus : `done: false, declined: true` ; délai dépassé :
  `done: false, timed_out: true`. Liste d'opérations : `done`, `applied` (opérations faites), `failed`
  (opération et erreur), `not_run`. Les erreurs de saisie restent des `ToolError` (`isError`).
- **Paramètres.** `reason` sur chaque écriture (montrée dans la confirmation). Références : `object` (nom ou
  numéro à partir de 1), `part`, `plate`, `filament_slot`, `toolhead`, tous numérotés à partir de 1.
- **Listes d'opérations.** Tout est vérifié avant la confirmation ; une seule confirmation ; une seule
  étape d'annulation ; arrêt à la première erreur.
- **Fichiers.** Chaque action de fichier accepte un `path` (absolu, `~` accepté) et le confirme par une
  question, avec un refus d'écraser sauf `overwrite: true`. Sans `path`, le dialogue du slicer s'ouvre.
  Ça rend aussi les exports testables sans dialogue natif.
- **Instructions.** Chaque domaine fournit son paragraphe d'instructions, assemblés à l'initialisation ;
  les détails d'usage vont dans la description de l'outil, pas dans les instructions.
- **Descriptions.** Une phrase sur le rôle, une ligne par action ; pas de redite des schémas.

## 4. Mode test et banc d'essai

- Option CMake `SNAPMAKER_AI_TEST_HOOKS`, activée seulement en `Debug` et jamais dans les paquets ni dans
  le workflow de release. Elle compile le mode test : la confirmation commune répond toute seule et une
  notification rouge permanente signale le mode. **Changé** : la réponse vient de la variable
  d'environnement `SNAPMAKER_AI_TEST_CONFIRM=yes|no` plutôt que d'une option de lancement, que l'analyse
  des arguments du slicer (upstream) refuserait sans nouveau point d'accroche.
- Script `scripts/fork/mcp-test.py` : lance le build de debug sur une configuration jetable (copie de
  test, port dédié, accès externe activé), joue des scénarios JSON (appel, résultat attendu), arrête
  l'instance par son PID et produit un compte rendu. Jamais d'imprimante réelle dans les scénarios par
  défaut ; ceux qui en ont besoin sont marqués et lancés à la demande.
- Scénarios de non-régression pour les 30 outils actuels, avant le regroupement : ils servent à vérifier
  que la phase 1 ne casse rien.

Vérifié en phase 0 : le build `Debug` passe avec `-j 8` (31 Go de RAM) sur les dépendances en `Release` ;
l'édition des liens (LTO) prend 5 à 10 minutes. Le slicer en Debug tranche lentement (30 à 60 s pour un
modèle simple). La coupe horizontale déclenche une assertion de `cut_mesh()` (upstream) en Debug sur des
cas que Release traite : son scénario est étiqueté `cut` et sauté par défaut.

## 5. Fonctions à ajouter

### Préréglages et filaments (`edit_presets`, `control_printer`)

- `select` : préréglage d'imprimante, de traitement, ou de filament pour un emplacement (comme les listes
  de la barre latérale). Le slicer demande lui-même quoi faire des modifications non enregistrées.
- `save` : enregistre le préréglage modifié sous un nom (préréglage utilisateur), sans dialogue.
- `add_slot`, `remove_slot`, `set_color` : comme les boutons de la liste des filaments ; supprimer un
  emplacement utilisé reporte ses objets sur un autre, à préciser dans la confirmation.
- `control_printer` `load_filament` : **à étudier d'abord** sur l'imprimante, avec toi, comme pour le
  déchargement (séquence `AUTO_FEEDING_BATCH`, états du module d'alimentation, cas des filaments souples).

### Couches et aperçu (`edit_objects`, `edit_plates`, `get_view`)

- Plages de hauteur : `add_height_range` (objet, de / à en mm, réglages) et `remove_height_range`, comme
  le menu *Plage de hauteur* de la liste des objets (`ModelObject::layer_config_ranges`).
- Événements par couche : `add_layer_event` (plaque, hauteur ou numéro de couche, `pause` ou
  `filament_change` vers un emplacement) et `remove_layer_event`, dans `Model::plates_custom_gcodes`, comme
  le curseur de l'aperçu. **Pas de G-code libre** : il partirait tel quel à l'imprimante, comme
  `post_process` déjà refusé.
- `get_view` `preview` : bascule sur l'aperçu, règle la plage de couches et capture le canevas
  (`ScreenCapture`). La vue change à l'écran pendant la capture : à dire dans la description.

### Annulation et calibrations (`undo`, `calibrate`)

- `undo` / `redo` : une ou plusieurs étapes, avec le nom des étapes dans la confirmation
  (`Plater::undo_to`, `redo_to`). Limité aux étapes de l'historique ; ne touche ni aux fichiers ni à
  l'imprimante.
- `calibrate` : crée le projet de calibration comme le menu *Calibration* (`Plater::calib_pa`,
  `calib_flowrate`, `calib_temp`, `calib_retraction`, `calib_max_vol_speed`, `calib_VFA`) avec ses
  paramètres. Nouveau projet : confirmation et proposition d'enregistrer. L'assistant guide ensuite la
  lecture du résultat avec les pages de calibration de la documentation.

### Texte, SVG, peinture automatique (`edit_objects`)

- `add_text` : texte en relief ou en creux sur un objet (police, taille, profondeur, position), par le
  moteur de l'outil *Texte* (tâche d'embossage). **À étudier** : appeler la tâche sans ouvrir le gizmo.
- `add_svg` : SVG ajouté comme pièce, pièce négative ou modificateur (l'import d'un SVG comme objet marche
  déjà par `edit_project` `import`).
- `paint_supports` : forcer ou bloquer les supports sur les surfaces plus inclinées qu'un angle donné
  (remplissage par angle de l'outil de peinture des supports) ; `clear_paint` efface une peinture. La
  peinture à la main reste écartée : le modèle ne peut pas viser des triangles.

## 6. Ordre de réalisation

Chaque phase se termine par : compilation, scénarios du banc d'essai, liste de vérifications à la main pour
toi, traductions des nouveaux textes, mise à jour de `FORK.md`, commit.

| Phase | Contenu |
|---|---|
| 0 ✓ | Base commune (confirmation, résultats, références), instructions par domaine, mode test et banc d'essai, scénarios des outils actuels |
| 1 ✓ | Regroupement vers la liste cible (sans fonction nouvelle) ; `path` pour les exports ; mise à jour des guides et notes qui citent d'anciens noms |
| 2 ✓ | Préréglages et filaments (sauf chargement) |
| 3 ✓ | Plages de hauteur, événements par couche, annulation |
| 4 ✓ | Aperçu tranché, calibrations |
| 5 (en partie) | Peinture automatique ✓ ; reste texte, SVG, et chargement du filament après étude sur l'imprimante |

## 6 bis. État et suite

Fait (phases 0 à 5 en partie) : 19 outils, conformes à la liste cible ; `tools/list` 23 624 octets,
instructions 6 290 octets. Banc d'essai : 115 étapes par défaut, toutes passées en Debug.

Non vérifié par le banc, à contrôler à la main sur le build Release :
- capture de l'aperçu (`get_view` preview, scénario `13-preview`, étiquette `screen`) : la fenêtre du slicer de
  test doit être visible à l'écran, ce que le banc ne garantit pas ;
- coupe et calibrations (`12-cut`, `14-calibrate`, étiquette `cut`) : `cut_mesh()` (upstream) lève une
  assertion en Debug sur des coupes que Release traite, et chaque calibration coupe son modèle. Piste :
  autoriser le mode test en `RelWithDebInfo` (sans assertions, jamais packagé) pour les jouer au banc ;
- imprimante réelle (`90-printer`, étiquette `printer`).

Reste à faire :
- `add_text` : texte en relief ou en creux par la tâche d'embossage, sans ouvrir le gizmo (à étudier) ;
- `add_svg` : SVG en pièce, pièce négative ou modificateur ;
- `control_printer` `load_filament` : à étudier sur l'imprimante avec l'utilisateur ;
- hauteur de couche variable (lissage adaptatif), non prévue au plan.

## 7. Risques

- **Traductions.** Les `.po` sont des fichiers upstream : chaque ajout en fin de fichier peut entrer en
  conflit à la synchronisation. À ajouter aux points d'accroche de `FORK.md`, avec la règle « ajouts en fin
  de fichier uniquement » ; à régénérer après chaque fusion si upstream relance `msgmerge`.
- **Commandes internes du slicer.** Plusieurs fonctions passent par des fonctions privées (`Plater::priv`)
  ou des gizmos ; on reste sur les fonctions publiques et les événements, quitte à reproduire quelques
  lignes (comme pour l'ajout de plaque), sans nouveau point d'accroche sauf nécessité notée dans `FORK.md`.
- **Imprimante.** Pause, reprise et arrêt (`printer.print.*`) ne sont pas encore vérifiés sur une vraie
  impression ; le chargement du filament demande une étude préalable.
- **Coût de contexte.** Fusionner réduit le nombre d'outils mais allonge certaines descriptions : on
  mesure la taille totale de `tools/list` avant et après la phase 1. Mesuré : 30 outils, 19 907 octets
  (instructions 6 015) avant ; 16 outils, 18 299 octets (instructions 5 804) après.
