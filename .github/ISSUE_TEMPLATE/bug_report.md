---
name: Bug report
about: Create a report to help us improve
title: ''
labels: Bug
assignees: ''

---

**Check List**
  - [ ] I checked my issue [doesn't exist yet](https://github.com/dokan-dev/dokany/issues?utf8=%E2%9C%93&q=is%3Aissue)
  - [ ] My issue is valid with mirror default sample and not specific to my user-mode driver implementation
  - [ ] I can always reproduce the issue with the provided description below.
  - [ ] I have updated Dokany to the [latest version](https://github.com/dokan-dev/dokany/releases) and have reboot my computer after.
  - [ ] I tested the latest [snapshot](https://github.com/dokan-dev/dokany/actions/workflows/build-and-test.yml?query=branch%3Amaster) (`DokanSetup-<version>.exe` / `dokan-<version>.zip` artifacts of the latest `master` build; GitHub sign-in is required to download Actions artifacts)

**Describe the bug**
A clear and concise description of what the bug is.

**To Reproduce**
Steps to reproduce the behavior:
1. Go to '...'
2. Click on '....'
3. Scroll down to '....'
4. See error

**Expected behavior**
A clear and concise description of what you expected to happen.

**Logs**
Please attach in separate files: mirror [output](https://github.com/dokan-dev/dokany/wiki/How-to-Debug-Dokan#mirrorfs), library logs and kernel [logs](https://github.com/dokan-dev/dokany/wiki/How-to-Debug-Dokan#get-logs).
In case of BSOD, please attach minidump or dump [analyze output](https://github.com/dokan-dev/dokany/wiki/How-to-Debug-Dokan#crash-report-bsod).

**Environment:**
- Windows version: 
- Processor architecture: 
- Dokany version: 
- Library type (Dokany/FUSE):

**Additional context**
Add any other context about the problem here.
