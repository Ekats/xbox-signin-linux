# Xbox Live sign-in for Age of Empires II: DE on Linux

Makes the **Sign in to Xbox Live** button work in AoE2DE under Proton, by
replacing the game's sign-in window with one that hands the login to your real
desktop browser.

If you have been staring at a blank white popup, or one that renders the
Microsoft login page and then does nothing forever, this is for you.

## Why it's broken

The game spawns `WebClient.exe <url> <targetUrl> <showUrlType>` — Microsoft's
XAL "TestWebClient" — and reads the resulting redirect URL from its **stdout**.
Two independent failures under Proton, and fixing either alone changes nothing:

**1. The redirect is never detected.** Decompiled (`monodis`), the whole thing
is:

```csharp
void OnNavigating(object sender, NavigatingCancelEventArgs e) {
    if (m_targetRedirect.IsMatch(e.Uri.OriginalString)) {
        e.Cancel = true;
        Console.Out.Write(e.Uri.OriginalString);   // bare URL, no newline
        Environment.Exit(0);
    }
}
```

`OnLoaded` subscribes to `WebBrowser.Navigating` before calling `Navigate`, so
it is armed from the start — but **wine-mono's WPF never raises that event**.
Verified: point it at a URL matching its own target regex and it neither prints
nor exits. Sign-in could never complete *even if the page rendered*.

**2. Wine's Gecko can't render the login.** It is 2.47.4, a Firefox 60-era
engine, and the newest wine-gecko that exists. It shows the Xbox splash and the
login form, then blanks partway through Microsoft's SPA.

Not the problem, for anyone else digging: WebView2/Edge (the game contains no
reference to it), wine-gecko missing (Proton ships it and it loads), your
distro's wine-mono (the game uses Proton's), user-agent or CSP overrides.

## How the fix works

`WebClient.exe` is replaced by a native program with the same stdout contract,
so the game cannot tell the difference. It writes `xal-request.txt` next to
itself, starts `xal-helper.py` out on the host, and waits for
`xal-result.txt`. The helper drives a throwaway Firefox over
[Marionette](#that-browser-is-under-remote-control-banner), watches for the
redirect, and writes it back.

The stdout contract, from the original's IL:

| condition | stdout | exit |
|---|---|---|
| target reached | the landing URL, bare, **no trailing newline** | 0 |
| cancelled | `USER_CANCEL`, bare | — |
| wrong arg count | *(stderr)* usage line | 1 |

`Console.Out.Write`, not `WriteLine` — a trailing newline breaks the game's
parsing.

Two things that make this harder than it looks. **Sign-in is multi-stage**:
XAL runs several `WebClient.exe` invocations (`login.live.com`, then
`sisu.xboxlive.com`), and the landing URL differs each time (`?code=...`,
`?status=success&...`, `?lc=...`), so the only valid test is the prefix match
the original used. And **Microsoft scrubs the landing URL** to `?removed=true`
via `history.replaceState` on arrival — which is why copying it from the
address bar cannot work. `replaceState` does not touch the
`PerformanceNavigationTiming` entry, so the helper recovers the real URL from
`performance.getEntriesByType("navigation")[0].name`.

## Install

Needs a mingw-w64 C compiler (Fedora `mingw64-gcc`, Debian
`gcc-mingw-w64-x86-64`, Arch `mingw-w64-gcc`), `python3`, and `firefox`.

```sh
git clone https://github.com/Ekats/aoe2de-xbox-signin-linux
cd aoe2de-xbox-signin-linux
./install.sh          # optional path arg; defaults to the usual Steam location
```

`install.sh` builds if needed, then backs the original up to
`WebClient.exe.orig`. Safe to re-run — it only takes a backup from a file that
imports `mscoree.dll`, so it cannot overwrite a good backup with the
replacement.

If you would rather not run a script: copy `WebClient.exe`, `xal-helper.py` and
`xal-launch.sh` into the game directory yourself, keeping a copy of the
original. Nothing else is needed — the replacement works out its own paths.

## Use

Launch the game and press **Sign in to Xbox Live**. A small window appears
in-game, Firefox opens, you sign in there, and both close themselves. All
stages are handled; the helper shuts down a couple of minutes after the last
one.

If no browser appears, start the helper by hand — it picks up the pending
request, and prints each stage as it goes:

```sh
python3 -u ~/.local/share/Steam/steamapps/common/AoE2DE/xal-helper.py
```

### That "Browser is under remote control" banner

The Firefox window shows a striped orange address bar. That is not a warning —
it is Firefox being upfront that a program is attached, which is the whole
mechanism. Over that connection the helper only ever: reads the current URL,
runs one line of JavaScript to recover the scrubbed URL, navigates to the next
stage, and asks the browser to quit. That is the complete list, about forty
lines of `xal-helper.py`.

It never touches your keystrokes, passwords, cookies, history, or your normal
Firefox. The window it drives is a **throwaway profile** created in `/tmp` and
deleted afterwards — which is also why you have to sign in inside it.

## Uninstall

```sh
./uninstall.sh
```

## Troubleshooting

Helper output is in `/tmp/xal-helper.log`, Firefox's in `/tmp/xal-firefox.log`.

- **Nothing happens on Sign In** — the helper never started. Run it by hand as
  above; the log says which directory it is watching.
- **Firefox doesn't open** — check that
  `firefox -no-remote -marionette -profile /tmp/ff-test -new-instance about:blank`
  works on its own.
- **Steam wiped it** — "Verify integrity of game files" restores the original.
  Re-run `./install.sh`, likewise after a game update.
- **Stale state** — `rm -f <game dir>/xal-{request,result}.txt`

## Other games

Any title shipping Microsoft's XAL `TestWebClient` should behave the same way;
the arguments and stdout contract come from XAL, not from AoE2DE. Untested
elsewhere.

## Tested on

Fedora 44 · Proton Experimental (wine-mono 11.2.0, wine-gecko 2.47.4) ·
AoE2DE via Steam (appid 813780) · Firefox

## Related

- [ValveSoftware/Proton#6481 — Microsoft games that require Xbox sign in](https://github.com/ValveSoftware/Proton/issues/6481)
- [Age of Empires forums — Switching AoE 2 Xbox live sign in to default browser](https://forums.ageofempires.com/t/switching-aoe-2-xbox-live-sign-in-to-default-browser/173332)

## License

GPL-3.0
