# Security policy

## What ocerz is, and is not

ocerz runs x86-64 Mac programs on Apple silicon, as the user who starts it and
with that user's full permissions. It is not a sandbox: a program run under
ocerz can do anything that program could do run natively, so "a guest program
can read my files" is expected behaviour and not a vulnerability.

What does count:

- A way for a program, a file or a library that ocerz only loads or inspects,
  rather than runs, to take control of ocerz. The Mach-O loader, the shared
  cache reader and the API database reader parse input they did not write.
- A way for one guest process to affect another it could not affect natively,
  or to gain rights the user who ran it does not have.
- A problem in what this repository publishes: a release binary, a build step,
  or anything that runs on a contributor's machine during `make` or `make check`.
- Memory corruption in ocerz itself that a guest program can trigger on purpose
  and that leads anywhere beyond a crash of that same program.

## Supported versions

Only the current `main` branch and the latest release are supported. Fixes are
not backported.

| Version | Supported |
| --- | --- |
| `main` (0.3-dev) | yes |
| 0.1 and earlier | no |

## Reporting a vulnerability

Report it privately through GitHub: on the repository page open **Security**,
then **Report a vulnerability**. Do not open a public issue, pull request or
discussion for it.

Please include:

- the ocerz version (`./ocerz version`) and the commit you built,
- the macOS version and the Mac it ran on,
- the mode (cache or `-native`) and the exact command,
- the smallest program or file that reproduces it, and what it achieves.

You will get an answer within seven days. A confirmed problem is fixed on
`main` and published as a GitHub security advisory that credits you, unless you
ask not to be named. Please allow ninety days, or until a fix is released if
that comes first, before disclosing it publicly.

## Out of scope

- Behaviour of Apple's own software that ocerz runs, which should be reported
  to Apple.
- Anything that needs the attacker to already run arbitrary code as the same
  user, outside ocerz.
- Crashes, hangs and wrong results with no security consequence. Those are
  ordinary bugs; open an issue.
