# Experience guides

Lessons learned from real prints, written from the confirmed records of the local
experience journal (MCP prompt `capitalize_experiences`) and reviewed before being
committed. `scripts/fork/build-setting-docs.py` bundles every `*.md` file of this
directory except this README into `../setting_docs.json`; the MCP server serves
them with `read_documentation` as pages of kind `experience`.

## Files

- One file per topic, named `exp_<topic>.md` (e.g. `exp_petg_stringing.md`), so
  that it never collides with an official documentation page.
- English, like the official documentation.
- No object names, file names, serial numbers or other personal data.

## Format

```markdown
# <Topic>

Scope: <printers, toolhead layout, materials, nozzles covered by this guide>

## <Lesson>

**Status:** Recommendation | Observation
**Conditions:** Snapmaker U1 (multi-toolhead), PETG on toolhead 2, 0.4 mm nozzle
**Symptom:** <what the user saw>
**Fix:** `setting_key`: old -> new (filament slot / toolhead when relevant), ...
**Evidence:** 4 prints (3 success, 1 partial), last 2026-10-27; records 20261027-213045-ab12cd, ...
**Notes:** <limits, side effects, contradicting cases>
```

## Rules

- **Observation**: a single confirmed case, or cases that do not all agree.
- **Recommendation**: at least 3 concordant confirmed cases and no contradicting one.
- A contradiction is written down in **Notes**, never resolved by guessing.
- When new evidence arrives, update the lesson's evidence and status rather than
  adding a duplicate lesson.
