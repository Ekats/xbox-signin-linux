#!/usr/bin/env python3
"""Host-side half of the AoE2DE Xbox Live sign-in workaround.

Runs OUTSIDE Wine, on the real desktop. The replacement WebClient.exe inside
the game's Proton prefix drops a request file; this program then:

  1. launches a dedicated Firefox with Marionette enabled
  2. opens the URL the game asked for
  3. watches that tab until it lands on the redirect target
  4. writes the landing URL back for the shim to hand to the game

Two details that are easy to get wrong:

* XAL runs the sign-in as SEVERAL stages, each a separate WebClient.exe
  invocation (auth on login.live.com, then sisu.xboxlive.com). This program
  therefore loops and keeps ONE browser alive across all of them so the
  session carries over. Exiting after the first stage leaves the game hanging.

* Microsoft rewrites the landing URL to "?removed=true" via
  history.replaceState almost immediately (an anti-phishing measure). That
  changes document.URL but NOT the PerformanceNavigation entry, so the
  original URL is recovered from performance.getEntriesByType("navigation").
  There is no race to lose.

The landing URL differs per stage -- "?code=..." on the auth stage,
"?status=success&..." later -- so the only test applied is the prefix match
the original program used, minus the scrubbed form.

Start it before pressing Sign In. Ctrl+C when the game is signed in.

  usage: xal-helper.py [--game DIR] [--keep-profile] [--once]
"""

import argparse
import fcntl
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

DEFAULT_GAME = os.environ.get(
    "XAL_GAME_DIR",
    os.path.expanduser("~/.local/share/Steam/steamapps/common/AoE2DE"))

MARIONETTE_PORT = 2828
FIREFOX_LOG = "/tmp/xal-firefox.log"

# Returns the URL the document was loaded with. replaceState does not alter
# the PerformanceNavigation entry, so this survives Microsoft's scrub.
RECOVER_JS = """
try {
  var e = performance.getEntriesByType('navigation');
  if (e && e.length && e[0].name) return e[0].name;
} catch (err) {}
return document.URL;
"""


class Marionette:
    """Minimal Marionette client. Framing is '<byte-length>:<json>'.

    Marionette is plain TCP with length-prefixed JSON, which is why this needs
    no third-party dependency -- unlike CDP, which would require a WebSocket.
    """

    def __init__(self, port, timeout=120):
        deadline = time.time() + timeout
        self.sock = None
        while time.time() < deadline:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), 2)
                break
            except OSError:
                time.sleep(0.3)
        if not self.sock:
            raise RuntimeError("could not connect to Marionette")
        # The 2s connect timeout above would otherwise apply to every recv on
        # this socket, and NewSession takes far longer than that.
        self.sock.settimeout(90)
        self.buf = b""
        self.msgid = 0
        self._recv()                      # server handshake
        self.call("WebDriver:NewSession", {})

    def _read_some(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise RuntimeError("Marionette closed the connection")
        return chunk

    def _recv(self):
        while b":" not in self.buf:
            self.buf += self._read_some()
        length, _, rest = self.buf.partition(b":")
        need = int(length)
        while len(rest) < need:
            rest += self._read_some()
        self.buf = rest[need:]
        return json.loads(rest[:need])

    def call(self, name, params):
        self.msgid += 1
        payload = json.dumps([0, self.msgid, name, params]).encode()
        self.sock.sendall(b"%d:%s" % (len(payload), payload))
        msg = self._recv()
        if len(msg) >= 4 and msg[2]:
            raise RuntimeError("%s: %s" % (name, msg[2]))
        return msg[3] if len(msg) >= 4 else None

    def url(self):
        return (self.call("WebDriver:GetCurrentURL", {}) or {}).get("value", "")

    def loaded_url(self):
        r = self.call("WebDriver:ExecuteScript",
                      {"script": RECOVER_JS, "args": []})
        return (r or {}).get("value", "") or ""

    def quit(self):
        """Ask the browser to exit. This is how Firefox gets shut down --
        killing the PID we spawned does not work, see run_firefox()."""
        try:
            self.call("Marionette:Quit", {"flags": ["eForceQuit"]})
        except (RuntimeError, OSError):
            pass

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def wait_for_request(path, timeout=None, poll=0.4):
    """Wait for the replacement to ask for a sign-in. Returns None on timeout.

    A timeout is used only after at least one stage has been handled: the
    stages of a sign-in arrive back to back, so a long quiet spell means the
    sequence is over and the helper (and its browser) should go away."""
    print("waiting for the game to request a sign-in ...")
    if timeout is None:
        print("  (press Sign In in the game now)")
    deadline = None if timeout is None else time.time() + timeout

    while True:
        if os.path.exists(path):
            with open(path, encoding="utf-8") as fh:
                lines = [ln.strip() for ln in fh if ln.strip()]
            if len(lines) >= 2:
                return lines[0], lines[1]
        if deadline is not None and time.time() > deadline:
            print("nothing further after %ds -- shutting down" % timeout)
            return None
        time.sleep(poll)


def run_firefox(url, profile):
    firefox = shutil.which("firefox")
    if not firefox:
        sys.exit("firefox not found in PATH")

    argv = [firefox, "-no-remote", "-marionette",
            "-profile", profile, "-new-instance", url]
    print("exec: %s" % " ".join(argv))

    log = open(FIREFOX_LOG, "w", encoding="utf-8")
    proc = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT)

    # NOTE: this PID is not a reliable handle on the browser. /usr/bin/firefox
    # hands off to another process, so it can exit while the browser is very
    # much alive. Shutdown therefore goes through Marionette:Quit in
    # stop_browser(), never through this PID.
    time.sleep(3)
    if proc.poll() is not None:
        print("launcher process exited (rc=%s); "
              "this is normal, waiting for Marionette" % proc.returncode)

    return proc


def make_profile():
    """Throwaway profile: Marionette must be reachable and the first-run pages
    must not sit in front of the login."""
    profile = tempfile.mkdtemp(prefix="xal-ff-")
    with open(os.path.join(profile, "user.js"), "w", encoding="utf-8") as fh:
        fh.write('user_pref("marionette.port", %d);\n' % MARIONETTE_PORT)
        fh.write('user_pref("browser.shell.checkDefaultBrowser", false);\n')
        fh.write('user_pref("datareporting.policy.'
                 'dataSubmissionPolicyBypassNotification", true);\n')
        fh.write('user_pref("browser.aboutwelcome.enabled", false);\n')
    return profile


def usable(url, target):
    """The original program matched the target prefix and nothing more. The
    only landing to refuse is Microsoft's anti-phishing scrub, which rewrites
    the URL to "?removed=true" and strips whatever XAL needed."""
    return url.startswith(target) and "removed=true" not in url


def stop_browser(mar, proc, profile, keep_profile):
    """Shut a stage's browser down and clean up after it.

    Shutdown goes through Marionette, never through the PID we spawned:
    /usr/bin/firefox hands off to another process, so that PID is not a handle
    on the browser. The pkill is a backstop -- the profile path is unique to
    this run, so anything still holding it is ours."""
    if mar is not None:
        mar.quit()
        mar.close()
    if proc is not None and proc.poll() is None:
        proc.terminate()
    if profile:
        subprocess.call(["pkill", "-f", profile],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1)
        if not keep_profile:
            shutil.rmtree(profile, ignore_errors=True)


def capture(mar, target):
    """Poll the browser until it lands on the target."""
    while True:
        try:
            current = mar.url()
        except (RuntimeError, OSError):
            print("browser closed before reaching the target")
            return ""
        if usable(current, target):
            return current
        if current.startswith(target):
            try:
                recovered = mar.loaded_url()
            except RuntimeError:
                recovered = ""
            if usable(recovered, target):
                return recovered
        time.sleep(0.25)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=DEFAULT_GAME,
                    help="game directory holding WebClient.exe")
    ap.add_argument("--keep-profile", action="store_true",
                    help="do not delete the throwaway Firefox profile")
    ap.add_argument("--once", action="store_true",
                    help="handle a single stage and exit (debugging)")
    ap.add_argument("--idle", type=int, default=120, metavar="SECONDS",
                    help="quit this long after the last stage (0 = never). "
                         "The first request is always waited for indefinitely.")
    args = ap.parse_args()

    req = os.path.join(args.game, "xal-request.txt")
    res = os.path.join(args.game, "xal-result.txt")

    # Only one helper at a time. This has to be a file lock rather than a
    # process check: the launcher that starts us runs inside pressure-vessel's
    # PID namespace and cannot see host processes, so it would start a fresh
    # helper -- and a fresh browser -- for every stage of the sign-in.
    lock_fh = open(os.path.join(args.game, "xal-helper.lock"), "w")
    try:
        fcntl.flock(lock_fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        print("another helper already holds the lock -- exiting")
        return

    profile = None
    proc = None
    mar = None

    handled = 0

    try:
        while True:
            # Indefinite until the first stage, so someone running this by
            # hand can take as long as they like getting to the button.
            timeout = args.idle if (handled and args.idle > 0) else None
            pending = wait_for_request(req, timeout)
            if pending is None:
                break
            auth_url, target = pending
            print("target: %s" % target)

            # A fresh browser for every stage. Reusing one window across
            # stages sounds tidier, but a reused window does not come to the
            # front over a fullscreen game -- the sign-in then looks dead
            # because nothing visibly happens. A new window takes focus, which
            # is what the player needs. Stages carry signed parameters, so
            # they do not depend on a shared browser session.
            stop_browser(mar, proc, profile, args.keep_profile)
            mar, proc = None, None

            profile = make_profile()
            proc = run_firefox(auth_url, profile)
            mar = Marionette(MARIONETTE_PORT)
            print("firefox launched")

            found = capture(mar, target)
            if not usable(found, target):
                found = ""

            with open(res, "w", encoding="utf-8") as fh:
                fh.write(found if found else "USER_CANCEL")
            try:
                os.remove(req)
            except OSError:
                pass

            print(("captured: " + found) if found else "cancelled")
            handled += 1

            if args.once:
                break
            print("---")
    except KeyboardInterrupt:
        print("\nstopping")
    finally:
        stop_browser(mar, proc, profile, args.keep_profile)


if __name__ == "__main__":
    main()
