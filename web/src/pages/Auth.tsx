// First boot (create the admin password) and sign-in.

import { useState } from "preact/hooks";
import type { AuthState } from "../types";
import { loadAuth, post } from "../store";
import { Logo } from "../icons";

function strength(p: string): number {
  let s = 0;
  if (p.length >= 8) s++;
  if (p.length >= 12) s++;
  if (/[a-z]/.test(p) && /[A-Z]/.test(p)) s++;
  if (/\d/.test(p)) s++;
  if (/[^A-Za-z0-9]/.test(p)) s++;
  return Math.min(s, 5);
}

export function Auth({ state }: { state: AuthState }) {
  const [pw, setPw] = useState("");
  const [pw2, setPw2] = useState("");
  const [msg, setMsg] = useState(state.locked ? "Too many attempts; try again in a few minutes." : "");
  const [busy, setBusy] = useState(false);
  const setup = state.setup_required;
  const s = strength(pw);

  const submit = async (e: Event) => {
    e.preventDefault();
    if (setup) {
      if (pw.length < 8) return setMsg("Use at least 8 characters.");
      if (pw !== pw2) return setMsg("The passwords don't match.");
    }
    setBusy(true);
    try {
      await post(setup ? "/api/setup" : "/api/login", { password: pw });
      setPw("");
      setPw2("");
      setMsg("");
      await loadAuth();
    } catch (x: any) {
      setMsg(x.message);
    } finally {
      setBusy(false);
    }
  };

  return (
    <div class="auth-wrap">
      <div class="dialog auth-card">
        <div class="brand"><Logo size={28} /><span>Stick Bridge</span></div>
        <p class="muted small" style={{ margin: 0 }}>{state.hostname} · firmware {state.version}</p>
        <form onSubmit={submit}>
          {setup ? (
            <>
              <h2>Create the admin password</h2>
              <div class="dialog-body">This first-boot step protects the dashboard, its settings and firmware updates. There's no default password. Powering the board on 5 times in a row, each time for under 10 seconds, clears it again.</div>
              <label class="dialog-field">
                <span>New password</span>
                <input type="password" autoComplete="new-password" value={pw} autoFocus onInput={(e) => setPw((e.target as HTMLInputElement).value)} />
                <div class="strength"><i style={{ width: `${s * 20}%`, background: s < 2 ? "var(--bad)" : s < 4 ? "var(--warn)" : "var(--ok)" }} /></div>
              </label>
              <label class="dialog-field">
                <span>Repeat password</span>
                <input type="password" autoComplete="new-password" value={pw2} onInput={(e) => setPw2((e.target as HTMLInputElement).value)} />
              </label>
            </>
          ) : (
            <>
              <h2>Sign in</h2>
              <label class="dialog-field">
                <span>Password</span>
                <input type="password" autoComplete="current-password" value={pw} autoFocus onInput={(e) => setPw((e.target as HTMLInputElement).value)} />
              </label>
            </>
          )}
          {msg && <div class="bad-text small">{msg}</div>}
          <div class="dialog-btns">
            <button class="btn primary" disabled={busy || !pw}>{setup ? "Create password" : "Sign in"}</button>
          </div>
        </form>
      </div>
    </div>
  );
}
