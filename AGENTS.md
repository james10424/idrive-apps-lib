# idrive-apps

Tools and apps for the iDrive 7 head unit (MGU) of a BMW, installed from the
mgu Store. Versions and CHANGELOG rules: `CONTRIBUTING.md`.

## Pull requests by AI agents

A pull request opened, or mostly written, by an AI agent says in its
description (`.github/pull_request_template.md` has the fields):

- **AI:** the tool and model, for example "Claude Code, Claude Opus 5.5" or
  "Codex, GPT-6.1". When several models worked on it, list each, with what
  it did if that helps.
- **Tested:** exactly how: which test suites ran and passed; whether it ran
  on a real car (which iDrive and firmware) or on a test unit; and what was
  not tested.
- **Human review:** whether a person reviewed it before it was opened.

A pull request without AI involvement says `AI: none`. CI fails a pull
request whose description has no `AI:` or `Tested:` field.
