# ADI agent helper

Build the helper with Node 22.19 or newer. From this directory, run `npm ci` and
`npm run build`. `npm test` runs the helper and loopback provider fixtures without
credentials.

The app host launches the absolute Node executable with the absolute path to
`dist/main.js` as an argument. `AppControl::checkAgent` checks one provider and
project for this app launch. `AppControl::runAgent` uses that same configuration
on a worker. The app has a fresh check result after each launch, and a changed
command or provider needs a new check. A check never sends project contents.

The helper uses JSON Lines on standard input and output. It supports a local
or LAN OpenAI-compatible endpoint, OpenAI, Anthropic, Google, and a faux provider
for tests. A provider credential, when required, comes from the operating
system credential store under service `org.adidaw.agent` and the configured
account. The launch configuration contains only the account name.
