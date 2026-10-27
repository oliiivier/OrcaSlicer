# Fork oliiivier/OrcaSlicer — Assistant IA

Fork de [Snapmaker/OrcaSlicer](https://github.com/Snapmaker/OrcaSlicer) qui ajoute un assistant LLM capable
d'analyser le projet et la configuration, de recommander des réglages et de les appliquer après confirmation.
Ce fork n'a pas vocation à être fusionné en amont : l'objectif est de pouvoir intégrer les mises à jour upstream
avec un minimum de conflits.

## Branches

| Branche              | Rôle                                                                  |
|----------------------|-----------------------------------------------------------------------|
| `main`               | Miroir exact de `upstream/main`. **Aucun commit du fork dessus.**     |
| `ai-assistant`       | Branche par défaut du fork : upstream + fonctionnalités du fork.      |
| `sync/upstream-*`    | Branches temporaires créées par le workflow de synchronisation.       |
| `feat/*`             | Développement des fonctionnalités, PR vers `ai-assistant`.            |

## Règles pour limiter les conflits

1. Tout le code du fork vit dans des dossiers qui n'existent pas upstream :
   - `src/slic3r/AIAssistant/` (C++ et `AIAssistant.cmake`)
   - `resources/web/ai_assistant/` (interface web)
   - `.github/workflows/fork-*.yml`
2. Les fichiers upstream ne sont touchés qu'aux **points d'accroche** ci-dessous, toujours entourés de
   `#ifdef SNAPMAKER_AI_ASSISTANT // fork`. Si un nouveau point est nécessaire, l'ajouter à cette liste.
3. Toujours intégrer upstream par **merge** (jamais de rebase de `ai-assistant`, jamais de squash des PR de sync).

### Points d'accroche dans le code upstream

| Fichier                         | Modification                                                  |
|---------------------------------|---------------------------------------------------------------|
| `src/slic3r/CMakeLists.txt`     | `include(AIAssistant/AIAssistant.cmake OPTIONAL)` après `encoding_check(libslic3r_gui)` |
| `src/slic3r/GUI/MainFrame.cpp`  | include de `AIAssistant.hpp` + appel `append_menu_items()` à la fin de `generate_help_menu()` |

Lister les écarts réels avec upstream (doit rester court) :

```bash
git diff upstream/main...ai-assistant --stat -- . ':!src/slic3r/AIAssistant' ':!resources/web/ai_assistant' ':!.github/workflows/fork-*' ':!FORK.md'
```

## Synchronisation avec upstream

### Automatique

`.github/workflows/fork-sync-upstream.yml` tourne chaque lundi (et à la demande depuis l'onglet Actions) :
- remet `main` à l'identique de `upstream/main` ;
- fusionne upstream dans une branche `sync/upstream-AAAAMMJJ` et ouvre une PR vers `ai-assistant` ;
- en cas de conflit, ouvre une issue listant les fichiers concernés.

Configuration requise sur GitHub :
- `ai-assistant` comme branche par défaut (les workflows planifiés ne tournent que depuis la branche par défaut) ;
- un secret `FORK_SYNC_TOKEN` : PAT fine-grained sur ce dépôt avec les droits *Contents*, *Pull requests*,
  *Issues* et *Workflows* en écriture.

### Manuelle

```bash
git fetch upstream
git switch main && git merge --ff-only upstream/main && git push origin main
git switch ai-assistant && git merge upstream/main
# résoudre les conflits éventuels (normalement limités aux points d'accroche), compiler, puis
git push origin ai-assistant
```

## Compilation

L'assistant est activé par défaut. Pour compiler sans lui (et vérifier que le fork reste neutre) :

```bash
cmake -DSNAPMAKER_AI_ASSISTANT=OFF ...
```
