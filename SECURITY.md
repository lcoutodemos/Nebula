# Security

## Reporting a vulnerability

Please don't report security issues in public issues or pull requests.

Report them privately through GitHub: open the repository's **Security** tab and choose
[**Report a vulnerability**](https://github.com/lcoutodemos/Nebula/security/advisories/new). Include
what you found, how to reproduce it, and which version or commit you tested. You'll get a reply as soon as
possible, and a fix will be credited to you unless you prefer otherwise.

## Scope

Nebula reads image files from folders you choose, runs a local AI model, and writes caches to your
local app data. Issues in how it parses images, model files, caches or library files are in scope.
Vulnerabilities in Windows, the Windows App SDK, ONNX Runtime or other dependencies should go to
their own maintainers.
