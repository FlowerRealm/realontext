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

1. **Symptoms only, no answer given away.** This is the hard requirement.
   Reject any report whose text names a source file path, a file
   name, or the function/method responsible for the defect. A reporter who
   writes "the bug is in `parser.c`" or "`resolve_alias()` returns NULL" has
   handed over the answer. Stack traces that merely pass through library
   frames are acceptable; a reporter pointing at the faulty code is not.
   When in doubt, reject.
2. **One concrete defect.** Something behaves wrongly and the report says what.
   Reject feature requests, questions, design discussions, tracking/meta issues,
   release checklists, and "several unrelated things" grab-bags.
3. **Actionable to an outsider.** An engineer who has never seen this codebase
   could read it and know what to go investigate. Concrete symptoms, versions,
   inputs, error text, reproduction steps all help.
4. **Self-contained.** Understandable without following links to other issues,
   pull requests, forum threads or chat logs.
5. **Written by a person.** Downweight template dumps with empty sections, and
   pure log dumps with almost no human description.
6. **Variety across your picks.** Different subsystems, different kinds of
   symptom. Avoid many near-duplicates of the same underlying problem.

A selection that clears criterion 1 by the reviewer's judgement is still
verified mechanically afterwards: the harness computes the ground truth and
checks the issue text against the real paths and function names. Picks that
fail that check are discarded and the next-ranked pick takes their place, so
ordering matters.

## Output

Write exactly one file, the path given in your task, containing only JSON:

```json
{
  "repo": "<the repo string given in your task>",
  "criteria_version": 2,
  "selected": [
    {"pr": 1234, "score": 5, "reason": "one short English sentence"}
  ]
}
```

- Exactly the number of entries your task asks for. If the input file has fewer
  lines than that, select every candidate that clears your bar and say so.
- Every `pr` must appear in the input file.
- `score` is 1-5: your confidence that this is a high-quality defect report that
  gives nothing away about the location of the fix.
- Order best first. Later entries are used as fallbacks, so the ordering matters.
  No prose outside the JSON.

Do not modify any other file.

## Report back

Only: how many you selected, the score distribution, and anything odd you
noticed about this project's issues.
