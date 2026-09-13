# Bug report selection brief

You are reviewing bug reports filed against an open-source project and picking
the best-written ones. Judge each report on its own merits as a piece of
technical writing about a defect.

## Input

One file, JSON Lines, one report per line, with fields:
`pr`, `issue`, `issue_title`, `issue_body`, `body_chars`.

The file is large. Read it in chunks with the Read tool (offset/limit, about 20
lines at a time) and keep a running shortlist. Do not read any other file in
that directory tree.

## Selection criteria, most important first

1. **One concrete defect.** Something behaves wrongly and the report says what.
   Reject feature requests, questions, design discussions, tracking/meta issues,
   release checklists, and "several unrelated things" grab-bags.
2. **Actionable to an outsider.** An engineer who has never seen this codebase
   could read it and know what to go investigate. Concrete symptoms, versions,
   inputs, error text, stack traces, reproduction steps all help.
3. **Self-contained.** Understandable without following links to other issues,
   pull requests, forum threads or chat logs.
4. **A report, not a diagnosis.** Prefer reports that describe symptoms.
   Downweight ones where the reporter already names the responsible
   function or file, or supplies the patch — those were written after the cause
   was known.
5. **Written by a person.** Downweight template dumps with empty sections, and
   pure log dumps with almost no human description.
6. **Variety across your picks.** Different subsystems, different kinds of
   symptom. Avoid many near-duplicates of the same underlying problem.

## Output

Write exactly one file, the path given in your task, containing only JSON:

```json
{
  "repo": "<the repo string given in your task>",
  "criteria_version": 1,
  "selected": [
    {"pr": 1234, "score": 5, "reason": "one short English sentence"}
  ]
}
```

- Exactly 50 entries. If the input file has fewer than 50 lines, select every
  candidate that clears your bar and say so in your report.
- Every `pr` must appear in the input file.
- `score` is 1-5: your confidence that this is a high-quality defect report.
- Order best first. No prose outside the JSON.

Do not modify any other file.

## Report back

Only: how many you selected, the score distribution, and anything odd you
noticed about this project's issues.
