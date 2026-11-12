// The code maker in tools/remote_code.html against the reference vector and tools/remote_code.py (which test_remote.c checks against the
// firmware): node test/host/test_remote_html.js
const fs = require("fs"), path = require("path"), { execFileSync } = require("child_process");
const html = fs.readFileSync(path.join(__dirname, "../../tools/remote_code.html"), "utf8");
const core = html.match(/<script id="core">([\s\S]*?)<\/script>/)[1];
const module_ = { exports: {} };
new Function("module", core)(module_);
const L = module_.exports;
let fails = 0, checks = 0;
const check = (ok, what) => { checks++; if (!ok) { fails++; console.log("FAIL " + what); } };

const k = Uint8Array.from({ length: 16 }, (_, i) => i);
check(L.siphash(k, Uint8Array.from({ length: 15 }, (_, i) => i)) === 0xa129ca6149be45e5n, "SipHash reference vector");
check(L.siphash(k, new Uint8Array(0)) === 0x726fdb47dd0e0e31n, "SipHash empty");

const py = path.join(__dirname, "../../tools/remote_code.py");
const cases = [
  ["K6ATV-10", "--chip", "2F0040001350524E34393620", "set moving on", 1794200000],
  ["K6ATV-10", "--secret", "my own secret", "set moving on", 1794200000],
  ["k6atv-2", "--chip", "2f004000 1350524e 34393620", "get   move_m", 1794200123],
  ["N0CALL", "--secret", "x", "set comment hello there friends", 4],
];
for (const [digi, kind, keyText, cmd, ctr] of cases) {
  const key = kind === "--chip" ? L.keyFromChip(keyText) : L.keyFromSecret(keyText);
  const command = L.tidy(cmd);
  const mine = `${ctr} ${command} ${L.code(key, digi, ctr, command)}`;
  const theirs = execFileSync("python3", [py, digi, kind, keyText, "--counter", String(ctr), cmd]).toString().trim();
  check(mine === theirs, `${digi} ${cmd}: page ${mine} / tool ${theirs}`);
}
let threw = false; try { L.keyFromChip("12345"); } catch (e) { threw = true; } check(threw, "short chip ID refused");
console.log(`${fails ? "FAILED" : "OK"}: ${checks} checks, ${fails} failed`);
process.exit(fails ? 1 : 0);
