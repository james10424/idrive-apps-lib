# Contributing

A pull request description has `AI:` and `Tested:` fields, and
`Human review:` when an AI agent wrote it (AGENTS.md, "Pull requests by
AI agents").

## Versions

Leave version numbers alone. The release gives each component it publishes
its next version and writes it into the tree. A pull request does not change
`"version"` in `component.json`, `mgu-app.json` or `package.json`, and an
app's package tree stays `apps/<id>/package/`. CI fails a pull request that
changes a version.

- **Patch** is the default.
- **Minor or major:** say so in the pull request; the maintainer adds the
  label `release:minor` or `release:major`. The label covers every component
  the pull request changes.

## CHANGELOG

A component with a CHANGELOG (an app's `package/CHANGELOG.md`) lists what
your pull request changes under `## Unreleased` at the top:

```md
# Changelog

## Unreleased

What changes, in a sentence or two for the people who use it.

## 0.6.2
...
```

The release moves those lines under the new version's heading and leaves an
empty `## Unreleased` for the next pull requests. CI fails a pull request
that changes such a component, its `release.json` included, without an
Unreleased entry.

When another pull request added its lines first, GitHub reports a conflict
in the CHANGELOG. Merge `main` into your branch locally
(`git merge origin/main`): the repository's `.gitattributes` keeps both
sets of lines, so there is nothing to resolve by hand.

## Requiring another component's next release

When your change needs a change to another component of this repository in
the same or an earlier pull request, require that component's next release
with `"next"` in `"requires"` (an app's `release.json`, a tool's
`component.json`):

```json
"requires": { "waze-on-cluster": "next" }
```

The release publishes both and writes the real version in. Keep a component
that is already released at its number (`"cluster-render": "0.4.0"`).

## New components

A new component starts at the version you give it, usually `0.1.0`; the
release publishes it as that.
