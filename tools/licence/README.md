# Licence keys (AceGears only)

AceStudio checks licence keys offline: each key is signed with AceGears' Ed25519 secret key,
and the app contains only the matching public key. `acestudio-licence` makes and checks keys.
It is built with the project (`build/tools/licence/`) and is **never** put in the installer.

## One-time setup

1. On a computer you trust, make the key pair:

       acestudio-licence new-keypair D:\AceGears\licence-keys

   This writes `licence-secret.key` (secret) and `licence-public.txt`. It never overwrites an
   existing pair.
2. Keep `licence-secret.key` offline and backed up (for example on two encrypted USB drives).
   Anyone with it can make keys. If you lose it, keys you have issued keep working, but you
   cannot make new ones for the same public key.
3. In GitHub, add a repository **variable** (not a secret: it is public by design) named
   `ACESTUDIO_LICENCE_PUBLIC_KEY` with the 64 hex digits from `licence-public.txt`. CI then
   builds releases that accept only your keys. Local builds take
   `-DACESTUDIO_LICENCE_PUBLIC_KEY=<hex>`.

Builds without that variable use the **development key**, whose secret half is in the source
code. They show "[development build …]" in Help › Licence and must not be sold.

## Issuing a key

    acestudio-licence issue --secret D:\AceGears\licence-keys\licence-secret.key ^
        --name "Customer Name" --email customer@example.com --log D:\AceGears\issued.csv

Options: `--edition standard` (default), `--expires YYYY-MM-DD` (subscriptions; default: never),
`--id` (default: a random `AG-…` reference). The key is printed on one line; send it to the
customer, who pastes it into Help › Licence or runs `acestudio-cli licence install <key>`.
`--log` appends the key's details (not the key) to a CSV file.

## Checking a key

    acestudio-licence verify "ACESTUDIO-1.…" --public <hex from licence-public.txt>

## Key format

`ACESTUDIO-1.` + base64url(JSON payload) + `.` + base64url(Ed25519 signature of everything before
the last dot). The payload has `product`, `id`, `name`, `email`, `edition`, `issued` and optionally
`expires`. See `core/include/acestudio/core/Licence.h`.

## Development and tests

`issue --secret dev` signs with the development key. Such keys work only in development-key
builds.
