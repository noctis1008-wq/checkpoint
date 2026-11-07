# Dropbox save backup and sync

The `dropbox` universal script backs up your saves, uploads Checkpoint's save
backups to your own Dropbox, and downloads the backups that only exist in Dropbox
(for example from another console) back onto this one. It runs on 3DS and Switch.

Copy [`common/universal/dropbox.c`](common/universal/dropbox.c) to
`/3ds/Checkpoint/scripts/universal/dropbox.c` (Switch:
`/switch/Checkpoint/scripts/universal/dropbox.c`). It needs Checkpoint 5.0 or
newer (the scripting engine). Download:
<https://github.com/noctis1008-wq/checkpoint/releases/latest/download/dropbox.c>.
Later updates can be fetched on the console itself with the
`c_github_downloader` script (same release).

**Nothing is ever written to a game's save by this script.** Downloaded backups
appear in Checkpoint's backup list; you restore them there, with Checkpoint's own
confirmation.

---

## One-time Dropbox setup

1. Open <https://www.dropbox.com/developers/apps> → **Create app**.
2. **Scoped access**, then **App folder** (the app can only see its own folder).
3. Give it a unique name → **Create app**.
4. **Permissions** tab: tick `files.metadata.read`, `files.content.write`,
   `files.content.read` → **Submit**.
5. **Settings** tab: note the **App key** and the **App secret**.
   If you will sign in with a different Dropbox account than the one that created
   the app, press **Enable additional users** there.

## Signing in (first run)

Run the script (**SELECT** → *Scripts* → `dropbox`).

1. Type the App key and the App secret. (Alternatively put
   `{"app_key": "...", "app_secret": "..."}` — or just the key — in
   `/3ds/Checkpoint/config/dropbox.json`; it is read once and deleted.)
2. The console shows a link, also saved in `config/dropbox_login.txt`:
   `https://www.dropbox.com/oauth2/authorize?client_id=<APP KEY>&response_type=code&token_access_type=offline`
   Open it on your phone or PC, sign in, press **Allow**.
3. Type the code Dropbox shows. Done: later runs sign in silently.

The app key, secret and the long-lived refresh token are stored only in
`config/dropbox.vault`, sealed with Checkpoint's `device_seal` (encrypted and tied
to this console; optionally also to a passphrase). See
[googledrive.md](googledrive.md#how-the-credentials-are-stored) for exactly what
that does and does not protect against.

## Using it

- **Sync all (upload + download)** — for every title: uploads the backups Dropbox
  does not have yet, downloads the Dropbox backups this console does not have.
  Saves and (3DS) extdata. Nothing that already exists is overwritten on either
  side, so running it again only moves what is new.
- **Back up all saves now, then sync** — first makes a fresh backup of every
  title's save (folder named `YYYYMMDD-HHMMSS`, like Checkpoint's own), then syncs.
- **This title: back up + sync** — the same for the highlighted title only.
- **Settings…** — check the account, set or remove the passphrase, sign out
  (revokes the access at Dropbox and deletes the vault).

Hold **B** to cancel at any time.

Dropbox layout, inside `Apps/<your app name>/`:

```
/3ds/saves/<TITLE ID> <title name>/<backup name>.zip
/3ds/extdata/<TITLE ID> <title name>/<backup name>.zip
```

Titles are matched by the title id in front of the folder name.

## Troubleshooting

- **"did not accept that code"** — codes work once and expire within minutes; run
  again. Also check the key and secret.
- **403** — a permission in step 4 is missing (change it, then sign out and in).
- **"no longer accepts the saved sign-in"** — access was revoked or the app
  deleted: Settings → Sign out, then sign in again.

## Testing on a PC

`tests/script-sim/run_dropbox.sh` runs this script with Checkpoint's real script
bindings against a local mock of the Dropbox API (sign-in, backup, upload,
download, pagination, Japanese title names, sign-out).

---

## 日本語の概要

セーブデータをバックアップ（抽出）し、Dropboxへアップロード、Dropboxにしかないバックアップをダウンロードするスクリプトです。

- `dropbox.c` を `SD:/3ds/Checkpoint/scripts/universal/` に置きます（Checkpoint 5.0以降）。
- 初回はApp keyとApp secretを入力し、表示されるリンクをスマホで開いて「許可」、表示されたコードを3DSに入力します。
- ゲームのセーブを直接書き換えることはありません。ダウンロードしたバックアップはCheckpointの一覧に入るので、そこで復元してください。
