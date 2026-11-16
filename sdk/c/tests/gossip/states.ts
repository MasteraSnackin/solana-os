// states.ts: scripted states of the gossip module (docs/GOSSIP.md section 10, in
// the parent repo), shared by the mock host (gossip-mock.ts) and the emulator
// test (lab-emu.ts), which hands the same tables to a test build of the Lab.
// Each state is what the module's reading functions would return:
//   me, programs, program[a], nearby, hearing, role, boss
// Times use each form a badge might give (lab_gossip.c reads them all): unix
// seconds, and server milliseconds as a decimal string.
import { createHash } from 'node:crypto';

export const NOW = Math.floor(Date.now() / 1000); // the mock's sys.time(), and near the emulator's clock

const B58 = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz';
export function b58(bytes: Uint8Array): string {
    let n = 0n;
    for (const x of bytes) n = n * 256n + BigInt(x);
    let out = '';
    while (n > 0n) {
        out = B58[Number(n % 58n)] + out;
        n /= 58n;
    }
    for (const x of bytes) {
        if (x) break;
        out = '1' + out;
    }
    return out;
}
export function unb58(t: string): Uint8Array {
    let n = 0n;
    for (const c of t) n = n * 58n + BigInt(B58.indexOf(c));
    const out: number[] = [];
    while (n > 0n) {
        out.unshift(Number(n % 256n));
        n /= 256n;
    }
    for (const c of t) {
        if (c !== '1') break;
        out.unshift(0);
    }
    return new Uint8Array(out);
}

// The contract's vectors, then made-up addresses (any 32 bytes decode to a genome).
export const V1 = '59qkXRBPMBADVrovwJYTsqXF21MvW9YYSGofQneQMruo';
export const V2 = 'G2QqkHaYPwAg64t6soMZvrDDT48gH3SF8DYsfutQ2JWC';
export const V3 = '31UFvJfpgAT4Paado1LNpEor9LL5DXMFAiCYzE2j5Cdj';
export const addr = (k: number) => b58(createHash('sha256').update(`gossip-lab test ${k}`).digest());

const TRAITS_LEFT = (t1: number, t2: number) => [0, 1, 2, 3, 4, 5].filter((t) => t !== t1 && t !== t2);
// docs/GOSSIP.md section 3, "Genome": the reference the C decoder is checked against.
export function genome(a: string) {
    const g = createHash('sha256').update(unb58(a)).digest();
    const t1 = g[3] % 6, t2 = (t1 + 1 + (g[4] % 5)) % 6;
    const traits = [t1, t2];
    if (g[5] < 64) traits.push(TRAITS_LEFT(t1, t2)[g[5] % 4]);
    const hue = g[0], h6 = hue * 6, region = h6 >> 8, f = h6 & 255, p = 64, q = 255 - ((191 * f) >> 8), t = 64 + ((191 * f) >> 8);
    const rgb = [[255, t, p], [q, 255, p], [p, 255, t], [p, q, 255], [t, p, 255], [255, p, q]][region];
    return {
        colour: (rgb[0] << 16) | (rgb[1] << 8) | rgb[2], body: g[1] & 7, eyes: (g[1] >> 3) & 7, marks: g[2] & 7, mark_seed: g[2] >> 3,
        traits, vir: 1 + (g[6] % 10), hard: 1 + (g[7] % 10), stab: 1 + (g[8] % 10),
        rarity: g[9] < 2 ? 'legendary' : g[9] < 14 ? 'epic' : g[9] < 54 ? 'rare' : 'common',
    };
}

type Prog = { a: string; par?: string; gen?: number; name?: string; verified?: boolean; spam?: boolean; nodes?: number };
const prog = (p: Prog) => ({ a: p.a, par: p.par ?? '', gen: p.gen ?? 0, name: p.name, verified: p.verified ?? true, spam: p.spam ?? false, ...genome(p.a), nodes: p.nodes });

export type State = {
    me: Record<string, unknown> | null;
    programs?: { slots: unknown[]; inbox: unknown[] };
    program?: Record<string, unknown>;
    nearby?: unknown[];
    hearing?: unknown[];
    role?: unknown;
    boss?: unknown;
};

const empty = (): State => ({
    me: {
        id: '7K2Q9M', on: true, synced: 0, incubating: 0, cls: '', clsAt: 0, stake: 0, rank: 0, credits: 0,
        slots: [false, false, false], act: '', inbox: [], held: 0, deployed: 0, deploys: 1, adopted: 0, boost: 0,
        fx: [], imm: [], items: [], itemsN: 0, epoch: { n: 0, ends: 0, obj: [] }, pub: false, earns: false,
        why: 'Link X at solana.com/breakpoint/badge/profile to earn',
    },
    programs: { slots: [false, false, false], inbox: [] },
    program: {},
    nearby: [],
    hearing: [],
});

const MINE = [prog({ a: V1, name: 'Turbine', nodes: 61 }), prog({ a: V2, par: V1, gen: 1, nodes: 52 }), prog({ a: V3, nodes: 3, spam: false }),
    prog({ a: addr(1), par: V3, gen: 1, verified: false, nodes: 0 }), prog({ a: addr(2), gen: 0, name: 'Sealevel', nodes: 140 })];
const INBOX = [prog({ a: addr(3), par: addr(9), gen: 4, nodes: 7 }), prog({ a: addr(4), spam: true, nodes: 30 }), prog({ a: addr(5), gen: 2, par: addr(8) })];
const KINDS = ['gossip.fee', 'gossip.qos', 'gossip.deny', 'gossip.close', 'gossip.space', 'gossip.cpi', 'gossip.upgrade', 'gossip.explorer',
    'gossip.buffer', 'gossip.relic', 'gossip.skin.aurora', 'gossip.fee'];
const NAMES = ['Priority Fee', 'Stake-weighted QoS', 'Deny-list', 'Close Account', 'Account Space', 'CPI', 'Upgrade Authority', 'Explorer',
    'Deploy Buffer', 'Genesis Relic', 'Aurora frame', 'Priority Fee'];
const ms = (secondsFromNow: number) => String((NOW + secondsFromNow) * 1000);

const busy = (): State => ({
    me: {
        id: '7K2Q9M', on: true, synced: NOW - 140, incubating: 0, cls: 'i', clsAt: ms(5 * 3600), stake: 340, rank: 1, credits: 4,
        slots: MINE.map((p) => p.a), act: V1, inbox: INBOX.map((p) => p.a), held: 14, deployed: 1, deploys: 1, adopted: 37, boost: 3,
        fx: [{ k: 'fee', until: ms(420) }, { k: 'explorer', until: NOW + 1500 }, { k: 'deny', until: ms(6000), p: addr(4) }],
        imm: [{ p: addr(6), until: NOW + 3 * 3600 + 300 }, { p: addr(7), until: ms(1800) }, { p: V2, until: NOW + 50 }, { p: addr(8), until: NOW + 9000 }],
        items: KINDS.map((k, i) => ({ id: `it_${String(i).padStart(2, '0')}`, k, n: NAMES[i] })), itemsN: 15,
        epoch: { n: 12, ends: ms(3 * 3600 + 5 * 60), obj: [
            { id: 'o1', text: 'Catch 3 programs', have: 1, need: 3 },
            { id: 'o2', text: 'Gossip to 5 nodes', have: 5, need: 5 },
            { id: 'o3', text: 'Hear a Leader', have: 0, need: 1 }] },
        pub: true, earns: true,
    },
    programs: { slots: MINE, inbox: INBOX },
    program: Object.fromEntries([...MINE, ...INBOX].map((p) => [p.a, p])),
    nearby: [],
    hearing: [],
});

const ROLES = [
    { id: 'R0LE01', name: 'Leader', kind: 'role', role: 'l', real: true, class: 'n', rssi: -52, st: addr(10), boost: 9 },
    { id: 'R0LE02', name: 'MEV Bot', kind: 'role', role: 'm', real: true, class: 'n', rssi: -71, st: '', boost: 0 },
    { id: 'R0LE03', name: '???', kind: 'role', role: 'a', real: false, class: 'n', rssi: -83, st: addr(11), boost: 3 },
];
const CLS = ['r', 'v', 'i', 'b', 'n'];
const nearbyList = () => [
    ...ROLES,
    ...Array.from({ length: 17 }, (_, k) => ({
        id: `B${String(k).padStart(2, '0')}X9Q`.slice(0, 6), name: k % 5 === 4 ? '' : `@node${k}`, kind: 'badge', role: '', real: false,
        class: CLS[k % 5], rssi: -45 - k * 3, st: k % 3 === 2 ? '' : addr(20 + k), boost: k % 4,
    })),
].sort((a, b) => b.rssi - a.rssi);

const hearingList = (exposed: boolean) => [
    { a: addr(20), pct: 72, state: 'hearing', from: 'B00X9Q', rssi: -48 },
    { a: addr(21), pct: 35, state: 'hearing', from: 'B01X9Q', rssi: -51 },
    { a: addr(10), pct: 18, state: 'hearing', from: 'R0LE01', rssi: -52 },
    { a: addr(23), pct: 5, state: 'hearing', from: 'B03X9Q', rssi: -60 },
    ...(exposed ? [
        { a: addr(24), pct: 100, state: 'exposed', from: 'B04X9Q', rssi: -57 },
        { a: addr(25), pct: 100, state: 'incubating', from: 'B05X9Q', rssi: -61 },
    ] : []),
];

export const STATES: Record<string, () => State> = {
    unlinked: () => ({ me: null }),
    nomodule: () => ({ me: undefined as never }), // gossip.me missing: the module isn't there
    empty,
    fresh: () => ({ ...empty(), me: { ...empty().me!, synced: NOW - 20 } }),
    busy,
    nearby: () => ({ ...busy(), nearby: nearbyList(), hearing: hearingList(false) }),
    exposure: () => {
        const s = busy();
        return { ...s, me: { ...s.me!, incubating: 1 }, nearby: nearbyList(), hearing: hearingList(true) };
    },
    offline: () => {
        const s = busy();
        return { ...s, me: { ...s.me!, synced: NOW - 3 * 3600, on: false } };
    },
};

// Events the module's on_gossip delivers (section 10), for scripts.
export const EVENTS: Record<string, Record<string, unknown>> = {
    exposed: { t: 'exposed', p: addr(24), from: 'B04X9Q' },
    adopted: { t: 'result', p: addr(25), r: 'adopted', stake: 2 },
    forked: { t: 'result', p: addr(24), r: 'forked', fork: addr(30) },
    seen: { t: 'result', p: addr(23), r: 'seen' },
    node: { t: 'node' },
    rank: { t: 'event', kind: 'rank', text: 'You reached Testnet' },
    milestone: { t: 'event', kind: 'milestone', text: 'Turbine reached 100 nodes' },
    drop: { t: 'event', kind: 'drop', text: 'You found a Priority Fee' },
    role: { t: 'role', offer: { sid: 7, ro: 'l', until: (NOW + 30) * 1000 } },
};
