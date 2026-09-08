# Windows binary updates

mozkey-date の Windows バイナリは GitHub Releases を更新元にします。

## Release を作る

`v` で始まるタグを push すると既存の Windows CI が Universal MSI をビルドし、次のファイルを GitHub Release に公開します。

- `MozkeyDate-Windows.msi`
- `MozkeyDate-Windows.msi.sha256`
- `update-mozkey-date.ps1`
- `update-mozkey-date.cmd`

例:

```powershell
git tag v0.1.0
git push origin v0.1.0
```

## Windows で更新する

MSI を一度インストールすると、スタートメニューの `Mozkey Date` フォルダーに **Mozkey Date を更新** が追加されます。これを実行すると、最新 Release の MSI を取得し、SHA-256 を検証してから更新します。更新スクリプトは MSI に同梱されるため、初回に手動で保存する必要はありません。

同じ Release を再インストールしたい場合は PowerShell から、インストール先の updater を `-Force` 付きで実行します。

```powershell
& "$env:ProgramFiles\Mozc\updater\update-mozkey-date.ps1" -Force
```

Release には `update-mozkey-date.ps1` と `update-mozkey-date.cmd` も引き続き添付するため、MSI をまだ導入していない環境では手動実行もできます。

無人インストールに寄せる場合は `-Quiet` も指定できます。UAC の昇格確認は Windows 側で表示されます。

## 更新方式

更新ツールは `%LOCALAPPDATA%\MozkeyDate\last-installed-release.txt` に、自身が最後に導入した Release タグを保存します。MSI は `REINSTALL=ALL REINSTALLMODE=vomus` を付けて起動するため、開発中に MSI の ProductVersion が同一でもバイナリを再配置できます。
