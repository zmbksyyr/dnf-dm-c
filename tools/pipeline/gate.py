#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Mechanical gate for the review loop.

Reads the four artefacts a round produces and answers one question:
does this round count as clean, must it continue, or must it be rolled back?

It is deliberately dumb: no network, no model, no judgement about code.
Only arithmetic over JSON plus a few consistency checks that catch the
cheap ways a round can lie to itself (score that doesn't match its own
breakdown, green verdict with a failing suite, an M-severity finding that
nobody resolved).

    python tools/pipeline/gate.py ../_pipeline/round-01
    python tools/pipeline/gate.py ../_pipeline/round-01 --record

Exit codes
    0  CLEAN     - every condition met, stop the loop
    1  CONTINUE  - no blocking damage, but not clean yet; run another round
    2  ROLLBACK  - this round broke something; revert before the next round
    3  BAD_INPUT - a file is missing or malformed; fix the round, don't judge it

Output is ASCII on purpose: this runs under cmd.exe with an OEM codepage.
"""

import json
import os
import sys
from datetime import datetime

CLEAN, CONTINUE, ROLLBACK, BAD_INPUT = 0, 1, 2, 3

PASS_SCORE = 9.0          # "clean" threshold, see README
SCORE_TOLERANCE = 0.15    # verdict.score vs weighted breakdown
BLOCKING_SEV = ("Critical", "Major")

REQUIRED = {
    "findings.json": ("round", "scope", "findings"),
    "fix.json": ("round", "build", "fixed"),
    "test.json": ("round", "build", "suites", "verdict"),
    "verdict.json": ("round", "score", "accept", "blocking", "regressions", "reason"),
}


def die(msg, code=BAD_INPUT):
    print("BAD_INPUT: " + msg)
    sys.exit(code)


def load(rdir, name):
    path = os.path.join(rdir, name)
    if not os.path.isfile(path):
        die("missing " + path)
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except json.JSONDecodeError as e:
        die("%s is not valid JSON: %s" % (name, e))
    except UnicodeDecodeError as e:
        die("%s is not UTF-8: %s" % (name, e))
    for key in REQUIRED[name]:
        if key not in data:
            die("%s lacks required field '%s'" % (name, key))
    return data


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = set(a for a in sys.argv[1:] if a.startswith("--"))
    if not args:
        die("usage: gate.py <round-dir> [--record]")
    rdir = os.path.abspath(args[0])
    if not os.path.isdir(rdir):
        die("not a directory: " + rdir)

    findings = load(rdir, "findings.json")
    fix = load(rdir, "fix.json")
    test = load(rdir, "test.json")
    verdict = load(rdir, "verdict.json")

    failures = []   # reasons the round is not clean
    damage = []     # reasons to roll back
    notes = []

    rounds = {findings.get("round"), fix.get("round"), test.get("round"), verdict.get("round")}
    if len(rounds) != 1:
        damage.append("round numbers disagree across artefacts: %s" % sorted(x for x in rounds if x is not None))

    # ---- build -----------------------------------------------------------
    brc = test.get("build", {}).get("rc")
    if brc != 0:
        damage.append("test.json build rc=%r (expected 0)" % (brc,))
    if fix.get("build") != "pass":
        damage.append("fix.json build=%r (expected 'pass')" % (fix.get("build"),))

    # ---- suites ----------------------------------------------------------
    suites = test.get("suites") or []
    if not suites:
        failures.append("no suites recorded - nothing was actually regressed")
    for s in suites:
        name = s.get("name", "?")
        if s.get("result") == "fail" or s.get("rc") not in (0, None):
            damage.append("suite %s failed (rc=%r)" % (name, s.get("rc")))
        elif s.get("result") == "blocked":
            failures.append("suite %s blocked - not verified" % name)

    # ---- regressions -----------------------------------------------------
    # A tester's "regression" is a claim, not a verdict. Only the judge decides
    # whether this round broke something. Treating the claim as damage would
    # roll back good work whenever a test happens to unearth an old defect.
    for r in (test.get("regressions") or []):
        failures.append("tester reported possible regression, judge must rule: " + str(r))
    for r in (test.get("pre_existing") or []):
        notes.append("pre-existing defect surfaced: " + str(r))
    for r in (verdict.get("regressions") or []):
        damage.append("judge confirmed regression: " + str(r))

    # ---- test verdict ----------------------------------------------------
    tv = test.get("verdict")
    if tv != "green":
        failures.append("tester verdict=%r" % (tv,))
    for b in (test.get("blocked") or []):
        failures.append("blocked: " + str(b))

    # ---- targeted evidence for each fixed item ---------------------------
    fixed_ids = [f.get("finding_id") for f in (fix.get("fixed") or []) if f.get("status") == "fixed"]
    targeted = {t.get("finding_id") for t in (test.get("targeted") or []) if t.get("result") == "pass"}
    for fid in fixed_ids:
        if fid not in targeted:
            failures.append("no passing targeted evidence for %s" % fid)

    # ---- unresolved Critical / Major ------------------------------------
    blocking = set(str(x) for x in (verdict.get("blocking") or []))
    sev = {f.get("id"): f.get("severity") for f in (findings.get("findings") or [])}
    resolved = set(fixed_ids)
    for u in (fix.get("unfixed") or []):
        if u.get("status") == "wontfix" and str(u.get("finding_id")) not in blocking:
            resolved.add(u.get("finding_id"))
    unresolved = [i for i, s in sev.items() if s in BLOCKING_SEV and i not in resolved]
    for i in unresolved:
        failures.append("unresolved %s: %s" % (sev[i], i))
    if unresolved:
        notes.append("severity gate capped the round: %d open Critical/Major" % len(unresolved))

    # ---- score -----------------------------------------------------------
    score = float(verdict.get("score", -1))
    if not (0.0 <= score <= 10.0):
        die("verdict.score out of range: %r" % (score,))
    bd = verdict.get("breakdown") or {}
    if bd:
        wsum = sum(float(v.get("weight", 0)) for v in bd.values())
        wscore = sum(float(v.get("weight", 0)) * float(v.get("score", 0)) for v in bd.values())
        if abs(wsum - 1.0) > 0.02:
            die("breakdown weights sum to %.3f, expected 1.0" % wsum)
        if abs(wscore - score) > SCORE_TOLERANCE:
            die("verdict.score %.2f does not match weighted breakdown %.2f (tol %.2f)"
                % (score, wscore, SCORE_TOLERANCE))
        score = round(wscore, 2) if abs(wscore - score) > 0.001 else score
    else:
        notes.append("no breakdown - score taken at face value")

    if score < PASS_SCORE:
        failures.append("score %.2f < %.1f" % (score, PASS_SCORE))
    if not verdict.get("accept"):
        failures.append("judge did not accept")

    # ---- spot checks -----------------------------------------------------
    sc = verdict.get("spot_checks") or []
    if len(sc) < 2:
        notes.append("only %d spot check(s); judge role asks for 2" % len(sc))

    # ---- decide ----------------------------------------------------------
    if damage:
        code, label = ROLLBACK, "ROLLBACK"
        reason = damage
    elif failures:
        code, label = CONTINUE, "CONTINUE"
        reason = failures
    else:
        code, label = CLEAN, "CLEAN"
        reason = ["all conditions met"]

    print("=" * 66)
    print("ROUND GATE  %s" % os.path.basename(rdir))
    print("=" * 66)
    print("findings   %d (C %d / M %d / m %d / i %d)" % (
        len(sev),
        sum(1 for s in sev.values() if s == "Critical"),
        sum(1 for s in sev.values() if s == "Major"),
        sum(1 for s in sev.values() if s == "Minor"),
        sum(1 for s in sev.values() if s == "Info")))
    print("fixed      %d, targeted evidence %d" % (len(fixed_ids), len(targeted)))
    print("suites     %d (%d pass)" % (len(suites), sum(1 for s in suites if s.get("result") == "pass")))
    print("score      %.2f  accept=%s" % (score, verdict.get("accept")))
    print("-" * 66)
    for r in reason:
        print("  - " + str(r))
    for n in notes:
        print("  * " + n)
    print("-" * 66)
    print("DECISION   %s   (exit %d)" % (label, code))

    if "--record" in flags:
        state_path = os.path.join(os.path.dirname(rdir), "state.json")
        state = {}
        if os.path.isfile(state_path):
            try:
                with open(state_path, "r", encoding="utf-8") as f:
                    state = json.load(f)
            except Exception:
                state = {}
        hist = state.get("history") or []
        entry = {
            "round": next(iter(rounds)),
            "score": score,
            "accept": bool(verdict.get("accept")),
            "decision": label,
            "open_critical": sum(1 for i in unresolved if sev.get(i) == "Critical"),
            "open_major": sum(1 for i in unresolved if sev.get(i) == "Major"),
            "at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        }
        hist = [h for h in hist if h.get("round") != entry["round"]] + [entry]
        hist.sort(key=lambda h: h.get("round", 0))
        state["history"] = hist
        state["last_round"] = entry["round"]
        state["clean"] = (code == CLEAN)
        state["updated"] = entry["at"]
        with open(state_path, "w", encoding="utf-8") as f:
            json.dump(state, f, ensure_ascii=False, indent=2)
            f.write("\n")
        print("state      %s" % state_path)

    sys.exit(code)


if __name__ == "__main__":
    main()
