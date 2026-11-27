#!/usr/bin/env node
// Keeps private details out of this public repository. It checks the
// tracked files, and the commit messages and author emails of a range, against a
// list of patterns that is itself private: CI reads it from the PRIVATE_PATTERNS
// secret, a local run from ~/.config/stick-bridge/private-patterns.txt. Each
// pattern is a JavaScript regex literal on its own line (/word/i); lines that
// start with # are comments.
//
//   node tools/privacy-check.mjs [--range <from>..<to>] [--text <file>|-] [--patterns <file>] [--allow-missing]
//
//   (no option)      the tracked files of the working tree
//   --range a..b     also the messages and author emails of the commits in a..b
//                    (a of all zeros, as on a new branch: b's own commit only)
//   --text f         only file f (- for stdin): an issue or PR body before it is posted
//   --patterns f     read the patterns from f
//   --allow-missing  no patterns is a skip, not a failure (a fork's pull request has no secrets)
//
// A hit is reported as where it is and which pattern number matched, never the text:
// the CI log is as readable as the repo. Exit 0 clean (or skipped), 1 on a hit, 2 on
// bad usage or a pattern list that won't parse.
import {spawnSync} from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import {fileURLToPath} from "node:url";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const LOCAL_PATTERNS = path.join(os.homedir(), ".config", "stick-bridge", "private-patterns.txt");

/** The patterns in `text`: one /regex/flags per line, # comments and blank lines skipped. */
export function parsePatterns(text) {
    const out = [];
    for (const [i, raw] of text.split(/\r?\n/).entries()) {
        const line = raw.trim();
        if (!line || line.startsWith("#")) continue;
        const m = /^\/(.+)\/([dgimsuvy]*)$/.exec(line);
        if (!m) throw new Error(`pattern line ${i + 1} is not a /regex/flags literal`);
        // A global or sticky regex keeps state between test() calls; a check must not.
        out.push(new RegExp(m[1], m[2].replace(/[gy]/g, "")));
    }
    return out;
}

/** Each line of `text` that a pattern matches, as {line, pattern} (1-based line, 0-based pattern). */
export function hitsIn(text, patterns) {
    const hits = [];
    text.split("\n").forEach((line, i) => {
        patterns.forEach((re, n) => {
            if (re.test(line)) hits.push({line: i + 1, pattern: n});
        });
    });
    return hits;
}

/** The hits in one file's bytes: its lines if it is text; a JPEG's Exif block (it can carry where a photo was taken). */
export function hitsInFile(name, buf, patterns) {
    if (buf.includes(0)) return /\.jpe?g$/i.test(name) && buf.includes("Exif") ? [{line: 0, pattern: -1, exif: true}] : [];
    return hitsIn(buf.toString("utf8"), patterns);
}

/** A hit as it may be printed: where, and which pattern - never the matched text. */
export function describe(where, hit) {
    if (hit.exif) return `${where}: photo metadata (Exif); strip it first`;
    return `${where}:${hit.line}: matches private pattern ${hit.pattern}`;
}

function git(args) {
    const r = spawnSync("git", ["-c", "safe.directory=*", ...args], {cwd: ROOT, encoding: "utf8", maxBuffer: 64 * 1024 * 1024});
    if (r.status !== 0) throw new Error(`git ${args.join(" ")} failed: ${(r.stderr || r.stdout).trim()}`);
    return r.stdout;
}

function loadPatterns(file) {
    if (file) return fs.readFileSync(file, "utf8");
    if (process.env.PRIVATE_PATTERNS?.trim()) return process.env.PRIVATE_PATTERNS;
    if (fs.existsSync(LOCAL_PATTERNS)) return fs.readFileSync(LOCAL_PATTERNS, "utf8");
    return undefined;
}

function main(argv) {
    const opts = {range: undefined, text: undefined, patterns: undefined, allowMissing: false};
    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        const value = () => {
            const v = argv[++i];
            if (v === undefined) throw new Error(`${a} needs a value`);
            return v;
        };
        if (a === "--range") opts.range = value();
        else if (a === "--text") opts.text = value();
        else if (a === "--patterns") opts.patterns = value();
        else if (a === "--allow-missing") opts.allowMissing = true;
        else throw new Error(`unknown argument ${a}`);
    }
    const source = loadPatterns(opts.patterns);
    if (source === undefined) {
        const msg = `privacy check: no patterns (PRIVATE_PATTERNS, ${LOCAL_PATTERNS} or --patterns)`;
        if (opts.allowMissing) {
            console.log(`${msg}; skipped.`);
            return 0;
        }
        console.error(msg);
        return 2;
    }
    const patterns = parsePatterns(source);
    if (patterns.length === 0) throw new Error("the pattern list is empty");
    const found = [];

    if (opts.text !== undefined) {
        const buf = opts.text === "-" ? fs.readFileSync(0) : fs.readFileSync(opts.text);
        for (const h of hitsInFile(opts.text, buf, patterns)) found.push(describe(opts.text === "-" ? "stdin" : opts.text, h));
    } else {
        const files = git(["ls-files", "-z"]).split("\0").filter(Boolean);
        for (const file of files) {
            const full = path.join(ROOT, file);
            if (!fs.existsSync(full) || !fs.statSync(full).isFile()) continue;
            for (const h of hitsInFile(file, fs.readFileSync(full), patterns)) found.push(describe(file, h));
        }
        if (opts.range) {
            const [from, to] = opts.range.split("..");
            if (!to) throw new Error("--range takes <from>..<to>");
            const commits =
                /^0+$/.test(from) || !from
                    ? [git(["rev-parse", to]).trim()]
                    : git(["rev-list", `${from}..${to}`])
                          .split("\n")
                          .filter(Boolean);
            for (const c of commits) {
                const text = git(["log", "-1", "--format=%an <%ae>%n%cn <%ce>%n%B", c]);
                for (const h of hitsIn(text, patterns)) found.push(describe(`commit ${c.slice(0, 7)} message`, h));
            }
        }
    }
    if (found.length) {
        console.error(`privacy check: ${found.length} hit(s)\n${found.join("\n")}`);
        return 1;
    }
    console.log(`privacy check: clean (${patterns.length} patterns)`);
    return 0;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    try {
        process.exitCode = main(process.argv.slice(2));
    } catch (e) {
        console.error(`privacy check: ${e.message}`);
        process.exitCode = 2;
    }
}
