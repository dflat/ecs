# ECS Library — RFC Index

RFCs (Requests for Comments) document design decisions, feature plans, and
architectural changes for the ECS library. Each RFC moves through a pipeline:

```
00-proposals → 01-active → 02-implemented
                         → 03-superseded
                         → 04-rejected
```

## Pipeline

| ID | Title | Status | Path |
|----|-------|--------|------|
| 0000 | Library Baseline | Baseline/Implemented | [02-implemented/0000-library-baseline.md](02-implemented/0000-library-baseline.md) |

## Workflow

1. **Draft** an RFC from `_TEMPLATE.md` and place it in `00-proposals/`.
2. **Discuss** — iterate on the design until the approach is agreed upon.
3. **Activate** — move to `01-active/` when implementation begins. Only one RFC
   should be active at a time.
4. **Complete** — move to `02-implemented/` when the work is done, tests pass,
   and docs are updated.
5. **Supersede/Reject** — if a proposal is replaced or abandoned, move it to
   `03-superseded/` or `04-rejected/` with a note explaining why.
