// lab-mock.ts: the Lab (gossip-lab, a C app in the parent repo) on the mock host,
// every screen walked by script against the scripted states of states.ts.
//
//   bun firmware/sdk/c/tests/gossip/lab-mock.ts [--app <gossip-lab dir>] [--only <scenario>]
//
// Builds the app with tools/cc/badge-cc, then runs mock-host.ts once per
// scenario (each a fresh app). Exit 1 when any scenario has a problem.
import { existsSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';

const args = Bun.argv.slice(2);
const opt = (k: string) => {
    const i = args.indexOf(k);
    return i >= 0 ? args[i + 1] : undefined;
};
const here = import.meta.dir;
const sdk = resolve(here, '../../../..');
const app = resolve(opt('--app') ?? join(sdk, '../server/content/apps/gossip-lab'));
if (!existsSync(join(app, 'main.c'))) {
    console.error(`lab-mock: no main.c in ${app} (pass --app)`);
    process.exit(2);
}
const wasm = join(mkdtempSync(join(tmpdir(), 'lab-mock-')), 'lab.wasm');
const cc = Bun.spawnSync([join(sdk, 'tools/cc/badge-cc'), app, '-o', wasm], { stdout: 'inherit', stderr: 'inherit' });
if (cc.exitCode !== 0) process.exit(1);

// [name, starting state, script]
const SCENARIOS: [string, string, string][] = [
    ['not linked, then linked', 'unlinked', 'screen:unlinked t:5 state:busy t:200 screen:home none'],
    ['no gossip module', 'nomodule', 'screen:unlinked t:20 none'],
    ['first launch: what it is, a class, the first deploy', 'fresh',
        'screen:intro k:select t:1 screen:intro sel:1 want:set_class t:1 k:select want:deploy screen:home'],
    ['first launch put off', 'fresh', 'screen:intro k:back screen:home none'],
    ['an empty node: every screen says so', 'empty',
        'screen:intro k:back screen:home sel:0 screen:node t:3 k:back sel:1 screen:slots sel:0 screen:slots k:back sel:2 screen:radar t:40 k:back ' +
        'sel:3 screen:hearing t:40 k:back sel:4 screen:items k:back sel:5 screen:timers t:40 k:back sel:6 screen:objectives t:40 k:back ' +
        'sel:7 screen:trophies k:back sel:8 screen:settings k:back screen:home none'],
    ['a busy node: slots, programs, place, drop, activate, name, parent', 'busy',
        'screen:home sel:0 screen:node t:5 sel:0 want:deploy sel:1 screen:settings k:back k:back screen:home ' +
        'sel:1 screen:slots t:5 sel:0 screen:program t:5 k:select screen:program sel:0 screen:pick sel:2 want:place ' +
        'sel:1 want:drop k:back k:back screen:slots sel:1 screen:program k:select sel:0 want:activate screen:program ' +
        'k:select sel:3 want:name sel:4 screen:program t:5 k:back k:back k:back screen:slots ' +
        'sel:5 screen:program k:select sel:0 screen:pick sel:0 want:place screen:program'],
    ['items: each kind and its picker', 'busy',
        'sel:4 screen:items sel:0 screen:item k:select want:use screen:items sel:2 screen:item k:select screen:pick sel:1 want:use ' +
        'screen:items sel:5 screen:item k:select screen:pick sel:0 screen:pick sel:0 want:use screen:items ' +
        'sel:3 screen:item k:select screen:pick sel:2 want:use screen:items sel:8 screen:item k:select want:deploy screen:items ' +
        'sel:10 screen:item k:select none k:back screen:items sel:6 screen:item k:select screen:pick sel:3 want:use screen:items'],
    ['timers, objectives, trophies, settings', 'busy',
        'sel:5 screen:timers t:60 k:back sel:6 screen:objectives t:60 k:back sel:7 screen:trophies k:back ' +
        'sel:8 screen:settings sel:1 want:public sel:2 want:lead sel:0 none sel:3 screen:settings sel:2 none'],
    ['the radar with 20 nodes and roles, one node, its program', 'nearby',
        'sel:2 screen:radar t:80 k:down k:down t:20 sel:0 screen:peer t:20 sel:0 screen:program k:back k:back ' +
        'sel:5 screen:peer t:20 k:back sel:19 screen:peer t:10 k:back k:back sel:3 screen:hearing t:80 none'],
    ['exposure and results arriving', 'exposure',
        'sel:3 screen:hearing t:40 ev:exposed ev:adopted t:20 ev:forked ev:seen ev:node ev:rank ev:milestone ev:drop ev:role t:40 ' +
        'k:back sel:7 screen:trophies k:back none'],
    ['gossip off and long unsynced', 'offline', 'screen:home t:40 sel:8 screen:settings sel:0 none'],
];

const only = opt('--only');
let failed = 0;
for (const [name, state, script] of SCENARIOS) {
    if (only && !name.includes(only)) continue;
    const r = Bun.spawnSync(['bun', join(sdk, 'sdk/c/tests/mock-host.ts'), wasm, '--mock', join(here, 'gossip-mock.ts'), '--keys', '',
        '--ticks', '2', '--state', state, '--script', script]);
    const out = r.stdout.toString() + r.stderr.toString();
    const problems = out.split('\n').filter((l) => l.includes('PROBLEM'));
    const screens = [...new Set(out.split('\n').map((l) => l.match(/\[app\] lab: (\S+)/)?.[1]).filter(Boolean))];
    const acts = out.split('\n').filter((l) => l.startsWith('gossip-mock: ') && l.includes('(')).map((l) => l.slice(13));
    if (r.exitCode !== 0 || problems.length) {
        failed++;
        console.log(`FAIL ${name}\n  ${problems.join('\n  ') || out.split('\n').slice(-15).join('\n  ')}`);
    } else {
        console.log(`ok   ${name}: ${screens.join(', ')}${acts.length ? `; ${acts.join(' ')}` : ''}`);
    }
}
console.log(failed ? `lab-mock: ${failed} scenarios failed` : 'lab-mock: every scenario passed');
process.exit(failed ? 1 : 0);
