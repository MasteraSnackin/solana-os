// gossip-mock.ts: the gossip module (docs/GOSSIP.md section 10) for mock-host.ts,
// with scripted states (states.ts) and a script of steps run after the keys.
//
//   bun tests/mock-host.ts app.wasm --mock tests/gossip/gossip-mock.ts --keys '' --ticks 2 \
//       --state busy --script 'screen:home sel:1 screen:slots ...'
//
// Steps (space separated):
//   k:<key>        on_key (up down left right select back)
//   t:<n>          n ticks
//   sel:<i>        the newest list's on_select, row i (from 0)
//   ev:<name>      on_gossip with states.ts EVENTS[name]
//   state:<name>   the module answers from another state from now on
//   screen:<name>  the app's last "lab: <name>" line names that screen
//   want:<fn>      the app called gossip.<fn> since the last want (acting calls)
//   none           no acting call since the last want
//   log:<text>     a console line containing text since the last log check
// Lists don't select by themselves here (mock-host's ui.list does): sel does it.
import { EVENTS, STATES, type State } from './states';

type Ctx = Record<string, any>;
let state: State;
let stateName = 'empty';
const acted: string[] = [];
const lines: string[] = [];
let logMark = 0;
let lastList: { f: any; n: number } | null = null;

const ACTING = ['set_class', 'activate', 'place', 'drop', 'deploy', 'use', 'name', 'public', 'lead', 'bid', 'claim', 'cure', 'vote',
    'role_answer', 'role_end', 'block', 'fork'];

export function install(c: Ctx) {
    const { api, fn, want, js2v, v2js, NIL, s, b, tbl, args } = c;
    const opt = (k: string, d: string) => {
        const i = args.indexOf(k);
        return i >= 0 ? args[i + 1] : d;
    };
    stateName = opt('--state', 'empty');
    if (!STATES[stateName]) throw new Error(`gossip-mock: no state ${stateName}`);
    state = STATES[stateName]();
    // Capture what the app prints (the screen names).
    const log = console.log;
    console.log = (...a: unknown[]) => {
        lines.push(a.join(' '));
        log(...a);
    };
    const read = (name: keyof State) => fn((a: any[]) => {
        if (name === 'me' && state.me === undefined) throw 'attempt to call a nil value';
        want(a, name === 'program' || name === 'genome' ? 's' : '', `gossip.${name}`);
        const v = name === 'program' ? (state.program ?? {})[c.str(a[0])] : state[name];
        return [v === undefined || v === null ? NIL : js2v(v)];
    });
    for (const n of ['me', 'programs', 'program', 'nearby', 'hearing', 'role', 'boss'] as const) api[`gossip.${n}`] = read(n);
    const specs: Record<string, string> = {
        set_class: 's', activate: 's', place: 's i', drop: 'i', deploy: 's?', use: 's t?', name: 's s', public: 'b', lead: 'b',
        bid: 's s b?', claim: 's i', cure: 's', vote: 'i i', role_answer: 'b', role_end: '', block: 't', fork: '',
    };
    for (const n of ACTING) {
        api[`gossip.${n}`] = fn((a: any[]) => {
            want(a, specs[n], `gossip.${n}`);
            const shown = a.map((x: any) => JSON.stringify(v2js(x))).join(', ');
            acted.push(`${n}(${shown})`);
            if (n === 'name' && c.str(a[1]).length > 16) return [NIL, s('too long')];
            if (n === 'place' && (a[1].v < 1 || a[1].v > 5)) return [NIL, s('no such slot')];
            return [b(true)];
        });
    }
    api['gossip.bids'] = fn(() => [tbl([])]);
    api['gossip.voters'] = fn(() => [NIL]);
    // ui.list without the stand-in's automatic selection; `sel` selects.
    api['ui.list'] = fn((a: any[]) => {
        want(a, 'i i i i t t?', 'ui.list');
        const o = a[5];
        const cb = o?.t === 5 ? o.v.get('on_select') : undefined;
        if (cb && cb.t !== 6) c.bug('ui.list: on_select is not a function');
        const rows = a[4].t === 5 ? a[4].v.size : 0;
        lastList = { f: cb ?? null, n: rows };
        return [c.obj('list', {
            items: (x: any[]) => (want(x, 'o t', 'list:items'), (lastList = { f: lastList?.f ?? null, n: x[1].v.size }), []),
            selected: (x: any[]) => (want(x, 'o', 'list:selected'), [c.i(1)]),
            select: (x: any[]) => (want(x, 'o i', 'list:select'), []),
        })];
    });
    // The theme's background reads black.
    api['ui.canvas'] = fn((a: any[]) => {
        want(a, 'i i i i t?', 'ui.canvas');
        return [c.obj('canvas', { pixel: () => [], get: () => [c.i(0)], clear: () => [] })];
    });
    // sys.time as the states expect.
    api['sys.time'] = fn(() => [c.i(1790000000)]);
}

export function script(c: Ctx) {
    const { run, deliver, ex, js2v, problems, args } = c;
    const i = args.indexOf('--script');
    const steps = (i >= 0 ? args[i + 1] : '').split(/\s+/).filter(Boolean);
    const keys: Record<string, number> = { up: 0, down: 1, left: 2, right: 3, select: 4, back: 5 };
    const screen = () => {
        for (let k = lines.length - 1; k >= 0; k--) {
            const m = lines[k].match(/\[app\] lab: (\S+)/);
            if (m) return m[1];
        }
        return '';
    };
    let mark = 0;
    for (const step of steps) {
        const [op, arg = ''] = [step.slice(0, step.indexOf(':') < 0 ? step.length : step.indexOf(':')), step.slice(step.indexOf(':') + 1)];
        const fail = (m: string) => problems.push(`script ${step}: ${m}`);
        if (op === 'k') {
            if (!(arg in keys)) fail('no such key');
            run(`on_key(${arg})`, () => ex.on_key(keys[arg], 0));
            deliver();
        } else if (op === 't') {
            for (let n = 0; n < Number(arg); n++) {
                run('on_tick', () => ex.on_tick(30));
                deliver();
            }
        } else if (op === 'sel') {
            if (!lastList?.f) fail('no list with on_select');
            else if (Number(arg) >= lastList.n) fail(`the list has ${lastList.n} rows`);
            else {
                const f = lastList.f;
                c.later(f, [Number(arg) + 1, NIL_SAFE(c)], 'list on_select');
                deliver();
            }
        } else if (op === 'ev') {
            if (!EVENTS[arg]) fail('no such event');
            else if (!ex.on_gossip) fail('the app exports no on_gossip');
            else {
                run(`on_gossip(${arg})`, () => ex.on_gossip(), [js2v(EVENTS[arg])]);
                deliver();
            }
        } else if (op === 'state') {
            if (!STATES[arg]) fail('no such state');
            else {
                stateName = arg;
                state = STATES[arg]();
            }
        } else if (op === 'screen') {
            if (screen() !== arg) fail(`on screen "${screen()}"`);
        } else if (op === 'want') {
            const got = acted.slice(mark);
            if (!got.some((x) => x.startsWith(`${arg}(`))) fail(`acting calls since the last check: ${got.join(' ') || 'none'}`);
            else console.log(`gossip-mock: ${got.join(' ')}`);
            mark = acted.length;
        } else if (op === 'none') {
            if (acted.length > mark) fail(`acting calls: ${acted.slice(mark).join(' ')}`);
        } else if (op === 'log') {
            const text = arg.replace(/_/g, ' ');
            if (!lines.slice(logMark).some((l) => l.includes(text))) fail('not printed');
            logMark = lines.length;
        } else fail('unknown step');
    }
    console.log(`gossip-mock: state ${stateName}, ${steps.length} steps, ${acted.length} acting calls`);
}

const NIL_SAFE = (c: Ctx) => c.NIL;
