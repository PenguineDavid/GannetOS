# Security Policy

## Project status

GannetOS is an experimental operating system. It is not currently
suitable for production use or sensitive data.

## Supported versions

Security fixes are currently considered for:

- The latest commit on `master`

Older commits and unofficial forks are not supported.

## Reporting a vulnerability

Please do not report security vulnerabilities in a public issue.

Use GitHub's private vulnerability reporting feature. Include:

- A clear description of the issue
- The affected component or commit
- Steps to reproduce it
- The expected and actual behavior
- Any proof-of-concept code or test case

Relevant issues include privilege escalation, ring-3 isolation bypasses,
kernel memory corruption, invalid pointer handling, filesystem corruption,
and network input vulnerabilities.

## Disclosure

Reports will be investigated when possible. A fix or mitigation may be
published through a commit, advisory, or release.

Because GannetOS is experimental and maintained as a personal project, no
specific response-time guarantee is provided.
