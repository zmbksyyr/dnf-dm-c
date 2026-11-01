#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Self-test for gate.py.

The gate is the only mechanical part of the loop: if it is wrong, every round
after it is wrong. These cases pin the four decisions and the anti-lying
checks (score vs breakdown, green verdict with a failing suite, targeted
evidence missing for a claimed fix).

    python tools/pipeline/test_gate.py

Exit 0 when every case behaves as expected.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GATE = os.path.join(HERE, "gate.py")


def findings(sev="Critical", extra=None):
    items = [{
        "id": "R1-001", "severity": sev, "file": "src/x.c", "line": 10,
        "title": "a real problem", "evidence": "x" * 40, "impact": "y" * 10,
        "confidence": "confirmed",
    }]
    if extra:
        items.append(extra)
    return {"round": 1, "scope": ["src/x.c"], "findings": items}


def fix(status="fixed"):
    return {"round": 1, "build": "pass",
            "fixed": [{"finding_id": "R1-001", "status": status, "files": ["src/x.c"], "note": "n"}]}


def test(**kw):
    suites = kw.pop("suites", [{"name": "v_gmwrite", "rc": 0, "result": "pass"}])
    return {"round": 1, "build": {"rc": kw.pop("build_rc", 0)},
            "suites": suites, "targeted": kw.pop("targeted", [
                {"finding_id": "R1-001", "test": "tools/v_r1.py", "rc": 0, "result": "pass"}]),
            "regressions": kw.pop("regressions", []), "blocked": kw.pop("blocked", []),
            "pre_existing": kw.pop("pre_existing", []),
            "verdict": kw.pop("verdict", "green")}


def bd(c=9.0, f=9.0, tc=9.0, cov=9.0, e=9.0):
    return {
        "缺陷真实性": {"weight": 0.25, "score": c},
        "修复正确性": {"weight": 0.30, "score": f},
        "测试证据": {"weight": 0.25, "score": tc},
        "覆盖广度": {"weight": 0.10, "score": cov},
        "工程规范": {"weight": 0.10, "score": e},
    }


def verdict(score=9.2, accept=True, blocking=None, regressions=None, breakdown="auto",
            spot=2):
    return {"round": 1, "score": score, "accept": accept,
            "blocking": blocking or [], "regressions": regressions or [],
            "reason": "because reasons", "next_focus": "n",
            "breakdown": bd(score, score, score, score, score) if breakdown == "auto" else breakdown,
            "spot_checks": [{"target": "t%d" % i, "what_i_did": "d", "result": "r"}
                            for i in range(spot)]}


CASES = [
    ("clean", 0, findings(), fix(), test(), verdict()),
    ("low_score", 1, findings(), fix(), test(), verdict(score=8.4, accept=False)),
    ("unresolved_major", 1,
     findings(sev="Major", extra={"id": "R1-002", "severity": "Major", "file": "src/x.c",
                                  "line": 20, "title": "second problem",
                                  "evidence": "z" * 40, "impact": "w" * 10,
                                  "confidence": "confirmed"}),
     fix(), test(), verdict(accept=False, blocking=["R1-002"])),
    ("wontfix_accepted_by_judge", 0,
     findings(sev="Major",
              extra={"id": "R1-002", "severity": "Major", "file": "src/x.c", "line": 20,
                     "title": "not worth fixing", "evidence": "z" * 40,
                     "impact": "w" * 10, "confidence": "unverified"}),
     {"round": 1, "build": "pass", "fixed": [{"finding_id": "R1-001", "status": "fixed"}],
      "unfixed": [{"finding_id": "R1-002", "status": "wontfix", "reason": "unreachable"}]},
     test(), verdict()),
    ("suite_failed", 2, findings(), fix(),
     test(suites=[{"name": "v_gmwrite", "rc": 1, "result": "fail"}]), verdict()),
    ("build_failed", 2, findings(), {"round": 1, "build": "fail", "fixed": []},
     test(build_rc=1), verdict()),
    ("regression_reported", 1, findings(), fix(), test(regressions=["coin no longer saves"]), verdict()),
    ("judge_confirmed_regression", 2, findings(), fix(), test(),
     verdict(regressions=["coin no longer saves"])),
    ("pre_existing_defect", 0, findings(), fix(),
     test(pre_existing=["clone hits a binary pk"]), verdict()),
    ("green_but_suite_fail", 2, findings(), fix(),
     test(suites=[{"name": "v_log", "rc": 0, "result": "fail"}], verdict="green"), verdict()),
    ("no_targeted_evidence", 1, findings(), fix(), test(targeted=[]), verdict()),
    ("breakdown_mismatch", 3, findings(), fix(), test(),
     verdict(score=9.2, breakdown=bd(5.0, 5.0, 5.0, 5.0, 5.0))),
]


def run_case(name, want, files):
    tmp = tempfile.mkdtemp(prefix="gate-%s-" % name)
    try:
        rdir = os.path.join(tmp, "round-01")
        os.makedirs(rdir)
        for fname, data in files.items():
            with open(os.path.join(rdir, fname), "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False)
        p = subprocess.run([sys.executable, GATE, rdir],
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
        ok = (p.returncode == want)
        print("%-28s want %d got %d  %s" % (name, want, p.returncode, "OK" if ok else "MISMATCH"))
        if not ok:
            print(p.stdout[-1500:])
            print(p.stderr[-800:])
        return ok
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    total = 0
    bad = 0
    for name, want, fnd, fx, tst, vd in CASES:
        total += 1
        if not run_case(name, want, {"findings.json": fnd, "fix.json": fx,
                                     "test.json": tst, "verdict.json": vd}):
            bad += 1
    print("-" * 40)
    print("%d/%d cases behave as designed" % (total - bad, total))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
