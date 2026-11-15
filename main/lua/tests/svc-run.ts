// Runs the background-service regression apps (t-svc*, and tests/c's t-c-svc
// when build.sh built it) in the emulator, headless, and checks what they
// print and what `svc` lists:
//   bun firmware/main/lua/tests/svc-run.ts
// Loads web/static/emulator/solana-os.js (bun run emulator) with the browser
// bits stubbed, like tests/c/run.ts. No server: server calls answer offline.
// The emulator's card is in memory, so a restart (services coming back at
// boot) is checked on a badge instead. Exit code 1 when anything is off.
import { existsSync, readdirSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';

const here = import.meta.dir;
const emuDir = join(here, '../../../../web/static/emulator');

const lines: string[] = [];
const g = globalThis as any;
g.window = globalThis;
g.location = {
    origin: 'http://127.0.0.1:9', href: 'http://127.0.0.1:9/emulator', protocol: 'http:', host: '127.0.0.1:9',
    hostname: '127.0.0.1', pathname: '/emulator', search: '',
};
const nvs = new Map<string, string>();
g.localStorage = {
    getItem: (k: string) => (nvs.has(k) ? nvs.get(k)! : null),
    setItem: (k: string, v: string) => void nvs.set(k, String(v)),
    removeItem: (k: string) => void nvs.delete(k),
    clear: () => nvs.clear(),
    key: (i: number) => [...nvs.keys()][i] ?? null,
    get length() { return nvs.size; },
};
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

const src = readFileSync(join(emuDir, 'solana-os.js'), 'utf8');
const SolanaOS = new Function(src + '; return SolanaOS;')();
const mod = await SolanaOS({
    locateFile: (p: string) => 'file://' + join(emuDir, p),
    print: (l: string) => void lines.push(l),
    printErr: (l: string) => void lines.push(l),
    server: 'http://127.0.0.1:9',
});
const cmd = (l: string) => mod.ccall('emu_console', null, ['string'], [l]);
for (let i = 0; i < 200 && !lines.some((l) => l.includes('console ready')); i++) {
    await sleep(50);
}
await sleep(500);

// Copy an app folder onto the emulated card: app.json, main.lua (or app.wasm), files/.
const mkdirp = (p: string) => {
    let cur = '';
    for (const part of p.split('/').filter(Boolean)) {
        cur += '/' + part;
        try { mod.FS.mkdir(cur); } catch { /* exists */ }
    }
};
const put = (id: string, dir: string) => {
    const dest = `/sdcard/hh/apps/${id}`;
    const walk = (from: string, to: string) => {
        mkdirp(to);
        for (const name of readdirSync(from)) {
            const p = join(from, name);
            if (statSync(p).isDirectory()) {
                walk(p, `${to}/${name}`);
            } else {
                mod.FS.writeFile(`${to}/${name}`, readFileSync(p));
            }
        }
    };
    walk(dir, dest);
};

let failures = 0;
const ok = (cond: boolean, what: string) => {
    console.log(`${cond ? 'ok  ' : 'FAIL'} ${what}`);
    if (!cond) failures++;
};
const since = (mark: number) => lines.slice(mark);
const waitFor = async (re: RegExp, ms: number, mark = 0) => {
    for (let t = 0; t < ms; t += 50) {
        const hit = since(mark).find((l) => re.test(l));
        if (hit) return hit;
        await sleep(50);
    }
    return undefined;
};
const svcList = async () => {
    const mark = lines.length;
    cmd('svc');
    await waitFor(/services, \d+ running/, 3000, mark);
    return since(mark);
};
const svcLine = async (id: string) => (await svcList()).find((l) => l.startsWith(`app:${id} `)) ?? '';

for (const id of ['t-svc', 't-svc-a', 't-svc-b', 't-svc-limits', 't-svc-mesh']) {
    put(id, join(here, id));
}
const cOut = join(here, 'c/out/t-c-svc');
const haveC = existsSync(join(cOut, 'app.wasm'));
if (haveC) put('t-c-svc', cOut);

// Open an app whose service.start asks the owner: SELECT on the badge's own dialog.
const openAndAllow = async (id: string, re: RegExp) => {
    const mark = lines.length;
    cmd(`lua run ${id}`);
    await sleep(1200);
    cmd('key select');                      // the OS's confirm: only a key the owner presses answers it
    return waitFor(re, 6000, mark);
};

// 1. Lifecycle, sandbox, messages, storage
let mark = lines.length;
const started = await openAndAllow('t-svc', /t-svc start: true/);
ok(!!started, 't-svc: service.start answered true after the owner allowed it');
ok(!!(await waitFor(/t-svc:svc forbidden: all nil/, 3000, mark)), 'service: ui, led, wifi, ble, ws, quest, spending, channel, mail.send are nil');
ok(!!(await waitFor(/t-svc:svc allowed: all there/, 3000, mark)), 'service: the allowed modules are there');
ok(!!(await waitFor(/t-svc second start: .*still waiting/, 3000, mark)), 'app: a second start while one waits is refused');
ok(!!(await waitFor(/t-svc big send: message over 1 KB/, 3000, mark)), 'app: service.send over 1 KB refused');
ok(!!(await waitFor(/t-svc:svc timers: .*too many timers \(4\)/, 3000, mark)), 'service: 4 timers at most');
const t = await waitFor(/t-svc:svc timer after (\d+) ms/, 4000, mark);
ok(!!t && Number(t!.match(/after (\d+) ms/)![1]) >= 990, `service: timers no faster than a second (${t})`);
mark = lines.length;
cmd('key select');
ok(!!(await waitFor(/t-svc from service: .*"echo":1/, 4000, mark)), 'app -> service -> app message round trip');
ok(!!(await waitFor(/flood queued [1-8] of 20/, 2000, mark)), 'app: a flood of service.send stops at the 8 waiting');
ok(!!(await waitFor(/t-svc:svc notify: true/, 6000)), 'service: a notice');
ok(!!(await waitFor(/t-svc:svc notify again: false/, 2000)), 'service: a second notice within 10 s is dropped');
await sleep(2500);
mark = lines.length;
cmd('key right');
const st = await waitFor(/t-svc storage: svc_ticks=(\d+)/, 2000, mark);
ok(!!st && Number(st!.match(/svc_ticks=(\d+)/)![1]) >= 3, `storage shared: the app reads the service's writes at once (${st})`);
mark = lines.length;
cmd('open home');
await sleep(2500);
let line = await svcLine('t-svc');
ok(/ on /.test(line) && /ticks \d+, app said hello/.test(line), `svc: still on with the app closed, the status the service set (${line})`);

// 2. Three at once, the fourth refused
await openAndAllow('t-svc-a', /t-svc-a start: true/);
cmd('open home');
await openAndAllow('t-svc-b', /t-svc-b start: true/);
cmd('open home');
mark = lines.length;
cmd('lua run t-svc-mesh');
await sleep(1200);
cmd('key right');                           // t-svc-mesh's service.start: refused before asking anyone
ok(!!(await waitFor(/t-svc-mesh start: false full/, 6000, mark)), 'a fourth service is refused: "full" (no dialog)');
cmd('open home');
const list = await svcList();
ok(list.filter((l) => /^app:\S+ +on /.test(l)).length === 3, `svc: three app services on\n${list.join('\n')}`);
cmd('heap');
await sleep(300);

// 3. Stops: the owner, the app
mark = lines.length;
cmd('svc stop app:t-svc-a');
await sleep(1500);
line = await svcLine('t-svc-a');
ok(/ off /.test(line) && / - /.test(line), `owner's stop: off, and no longer starts at boot (${line})`);
mark = lines.length;
cmd('lua run t-svc');
await sleep(1200);
cmd('key left');
ok(!!(await waitFor(/t-svc:svc on_stop after/, 4000, mark)), "app's service.stop: on_stop ran");
cmd('open home');
cmd('svc stop app:t-svc-b');
await sleep(1500);

// 4. Hostile services: each ends with its reason; the app on screen keeps running
const hostile: [string, string, RegExp][] = [
    ['up', 'loop', /Failed: service\.lua:\d+: Service took too long/],
    ['down', 'alloc', /Failed: The service needs more than 1 MB/],
    ['left', 'pattern', /Failed: service\.lua:\d+: Service took too long/],
];
await openAndAllow('t-svc-limits', /t-svc-limits spam: start true/);
await sleep(2500);
line = await svcLine('t-svc-limits');
ok(/spammed [0-8]\b/.test(line), `spam: a service's sends to the app stop at the app's queue (${line})`);
for (const [key, mode, re] of hostile) {
    mark = lines.length;
    cmd(`key ${key}`);
    await waitFor(new RegExp(`${mode}: start true`), 6000, mark);
    const t0 = Date.now();
    await waitFor(/Failed: /, 8000, mark);
    line = await svcLine('t-svc-limits');
    ok(re.test(line) && /failed/.test(line), `${mode}: ${line} (${Date.now() - t0} ms)`);
}
mark = lines.length;
cmd('key select');                          // stopper: on_stop never returns
await waitFor(/stopper: start true/, 6000, mark);
const t1 = Date.now();
cmd('svc stop app:t-svc-limits');
await waitFor(/t-svc-limits:svc on_stop: looping forever/, 3000, mark);
for (let i = 0; i < 40 && / stopping /.test(await svcLine('t-svc-limits')); i++) await sleep(100);
line = await svcLine('t-svc-limits');
ok(/ off /.test(line), `stopper: on_stop that never returns still ends (${Date.now() - t1} ms): ${line}`);
const alive = since(mark).some((l) => /lua:t-svc-limits/.test(l));
ok(!alive, 'the app on screen never failed meanwhile');
cmd('open home');

// Work in C that VM instructions don't count: the second per callback stops it.
mod.FS.writeFile('/sdcard/hh/apps/t-svc-limits/data.json', '{"mode":"rep"}');
mark = lines.length;
await openAndAllow('t-svc-limits', /rep: start true/);   // the owner stopped it above: it asks again
const t2 = Date.now();
await waitFor(/Failed: /, 8000, mark);
line = await svcLine('t-svc-limits');
ok(/Service took too long/.test(line), `rep: ${line} (${Date.now() - t2} ms)`);
cmd('open home');

if (process.argv.includes('--minute')) {
    await openAndAllow('t-svc-limits', /start true/);
    mark = lines.length;
    cmd('key right');
    await waitFor(/Failed: /, 70000, mark);
    line = await svcLine('t-svc-limits');
    ok(/used too much of the processor/.test(line), `minute: ${line}`);
    cmd('open home');
}

// 5. A C app's service
if (haveC) {
    mark = lines.length;
    const cs = await openAndAllow('t-c-svc', /ok t-c-svc service.start answered true/);
    ok(!!cs, 'C: service_start answered true while the app holds the runtime');
    line = await svcLine('t-c-svc');
    ok(/Starts when the C app closes/.test(line), `C: waits for the app to close (${line})`);
    cmd('open home');
    ok(!!(await waitFor(/t-c-svc svc: started/, 5000, mark)), 'C: the service instance started at on_service_start once the app closed');
    ok(!!(await waitFor(/ok t-c-svc svc: the screen, spending and the radio's channel are nil/, 3000, mark)), 'C: forbidden modules are nil');
    await sleep(2500);
    line = await svcLine('t-c-svc');
    ok(/ on /.test(line) && /C ticks \d+/.test(line), `C: running with the app closed (${line})`);
    // A C app opened while a C service runs: the service pauses, then comes back.
    mark = lines.length;
    cmd('lua run t-c-svc');
    await waitFor(/Paused: a C app is open|t-c-svc app:/, 5000, mark);
    await sleep(1500);
    line = await svcLine('t-c-svc');
    ok(/Paused: a C app is open|Starts when the C app closes/.test(line), `C: paused while a C app holds the runtime (${line})`);
    cmd('open home');
    await sleep(3000);
    line = await svcLine('t-c-svc');
    ok(/ on /.test(line), `C: resumed after the C app closed (${line})`);
    cmd('svc stop app:t-c-svc');
    await sleep(1500);
    ok(!!since(mark).find((l) => /t-c-svc svc: on_stop after ticks/.test(l)), 'C: on_stop ran');
} else {
    console.log('skip C (bash firmware/main/lua/tests/c/build.sh first)');
}

// 6. Removal forgets it
cmd('svc stop app:t-svc');
await sleep(1500);
cmd('lua rm t-svc');
await sleep(500);
line = await svcLine('t-svc');
ok(line === '', 'lua rm: the service is forgotten');

if (process.argv.includes('--log')) console.log(lines.join('\n'));
console.log(failures ? `${failures} FAILED` : 'all ok');
process.exit(failures ? 1 : 0);
