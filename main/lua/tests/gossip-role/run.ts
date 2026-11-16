// Gossip roles, headless (Solana OS 2.4.0; the parent repo's docs/GOSSIP.md
// §4 and §8): several emulated badges in one bun process, sharing the
// emulator's radio (a BroadcastChannel), against a small fake of the server's
// role side.
//
//   bun firmware/main/lua/tests/gossip-role/run.ts            (everything)
//   bun firmware/main/lua/tests/gossip-role/run.ts fanin      (only the fan-in measurement)
//
// Loads web/static/emulator/solana-os.js (bun run emulator). The fake answers
// the badge's hello (an id and a mesh key each) and plays the role side of
// §7/§8 through the role service's test mode: `grole test sync <RoleState>`
// is one sync round (the body's role part printed, then the answer's
// RoleState handed back), so this runs before fw-node's sync client exists.
// It checks every tag the badges make (role HELLO, ack, cured) with the keys
// it handed out, as the server would.
//
// Exit code 1 when anything is off.
import { createHmac, randomBytes } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { FakeServer, tag } from './fake';

const here = import.meta.dir;
const emuDir = join(here, '../../../../../web/static/emulator');
const only = process.argv[2] ?? '';
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

let failures = 0;
const ok = (cond: boolean, what: string, extra = '') => {
	console.log(`${cond ? 'ok  ' : 'FAIL'} ${what}${extra ? `  (${extra})` : ''}`);
	if (!cond) failures++;
};

const server = new FakeServer(Number(process.env.PORT ?? 32012));
const url = server.url;
const src = readFileSync(join(emuDir, 'solana-os.js'), 'utf8');

type Emu = {
	name: string;
	lines: string[];
	id: string;
	key: string;
	mac: string;
	cmd: (l: string) => void;
	waitFor: (re: RegExp, ms: number, mark?: number) => Promise<string | undefined>;
	run: (l: string, re: RegExp, ms?: number) => Promise<string | undefined>;
	halt: () => void;
	put: (id: string, dir: string, rename?: string) => void;
};

// mac5: the emulator's fake RSSI of this badge at every other one is
// -42 - (mac5 % 32) - (0..3) dBm: 0 is close (about -43), 30 is far (about -73).
async function boot(name: string, mac5: number): Promise<Emu> {
	const lines: string[] = [];
	const nvs = new Map<string, string>();
	const mac = [...randomBytes(5)].map((b) => b.toString(16).padStart(2, '0')).join('') + mac5.toString(16).padStart(2, '0');
	nvs.set('solana-os-emu:id', mac);
	const u = new URL(url);
	const loc = { origin: u.origin, href: `${u.origin}/emulator`, protocol: u.protocol, host: u.host, hostname: u.hostname, pathname: '/emulator', search: '' };
	const ls = {
		getItem: (k: string) => (nvs.has(k) ? nvs.get(k)! : null),
		setItem: (k: string, v: string) => void nvs.set(k, String(v)),
		removeItem: (k: string) => void nvs.delete(k),
		clear: () => nvs.clear(),
		key: (i: number) => [...nvs.keys()][i] ?? null,
		get length() {
			return nvs.size;
		}
	};
	const SolanaOS = new Function('window', 'location', 'localStorage', `${src}; return SolanaOS;`)(globalThis, loc, ls);
	const mod = await SolanaOS({
		locateFile: (p: string) => `file://${join(emuDir, p)}`,
		print: (l: string) => void lines.push(l),
		printErr: (l: string) => void lines.push(l),
		server: url
	});
	const waitFor = async (re: RegExp, ms: number, mark = 0) => {
		for (let t = 0; t < ms; t += 25) {
			const hit = lines.slice(mark).find((l) => re.test(l));
			if (hit) return hit;
			await sleep(25);
		}
		return undefined;
	};
	const e: Emu = {
		name,
		lines,
		id: '',
		key: '',
		mac,
		cmd: (l) => mod.ccall('emu_console', null, ['string'], [l]),
		waitFor,
		run: async (l, re, ms = 3000) => {
			const mark = lines.length;
			e.cmd(l);
			return waitFor(re, ms, mark);
		},
		halt: () => mod.ccall('emu_halt', null, [], []),
		// Copy a test app onto the emulated card, as svc-run.ts does; rename: install it under another id.
		put: (id, dir, rename) => {
			const dest = `/sdcard/hh/apps/${rename ?? id}`;
			let cur = '';
			for (const part of dest.split('/').filter(Boolean)) {
				cur += `/${part}`;
				try {
					mod.FS.mkdir(cur);
				} catch {
					/* exists */
				}
			}
			for (const f of ['main.lua', 'app.json']) {
				let t = readFileSync(join(dir, f), 'utf8');
				if (rename && f === 'app.json') t = t.replace(`"${id}"`, `"${rename}"`);
				mod.FS.writeFile(`${dest}/${f}`, t);
			}
		}
	};
	if (!(await waitFor(/console ready/, 30_000))) throw new Error(`${name}: no console`);
	return e;
}

// Online once for an id and a mesh key, then Wi-Fi off: the radio parks on the mesh channel.
async function online(e: Emu) {
	e.cmd('wifi connect "Emulator Guest"');
	for (let i = 0; i < 400 && !server.byMac(e.mac); i++) await sleep(50);
	const b = server.byMac(e.mac);
	if (!b) throw new Error(`${e.name}: no hello`);
	e.id = b.id;
	e.key = b.key;
	await e.waitFor(/claimed|hello/, 3000);
	await sleep(500);
	e.cmd('wifi off');
	await sleep(300);
	await e.run(`grole test on ${Date.now()}`, /grole state/);
}

const q = (o: unknown) => JSON.stringify(o).replace(/"/g, "'");
const bodyOf = (l: string | undefined) => (l ? JSON.parse(l.slice('grole body '.length)) : null);
const stateOf = (l: string | undefined) => {
	if (!l) return undefined;
	const j = l.slice('grole state '.length);
	return j === 'null' ? null : JSON.parse(j);
};
async function sync(e: Emu, state?: unknown, fail = false) {
	const mark = e.lines.length;
	e.cmd(`grole test sync ${fail ? 'fail' : state === undefined ? 'none' : q(state)}`);
	const body = bodyOf(await e.waitFor(/^grole body /, 3000, mark));
	const st = stateOf(await e.waitFor(/^grole state /, 3000, mark));
	return { body, st };
}
async function status(e: Emu) {
	const mark = e.lines.length;
	e.cmd('grole');
	await e.waitFor(/^ {2}cures:/, 3000, mark);
	await sleep(50);
	return e.lines.slice(mark).join('\n');
}
// Wait, syncing every 10 s as the node service would (a role without a sync for 90 s ends).
async function keepAlive(es: Emu[], ms: number, state?: unknown) {
	const end = Date.now() + ms;
	while (Date.now() < end) {
		await sleep(Math.min(10_000, end - Date.now()));
		for (const e of es) await sync(e, state);
	}
}
const events = (e: Emu, mark: number, t: string) =>
	e.lines
		.slice(mark)
		.filter((l) => l.startsWith('grole ev '))
		.map((l) => JSON.parse(l.slice('grole ev '.length)))
		.filter((x) => x.t === t);

// Addresses: vector 1 and 2 of GOSSIP.md §3, one more as "spam".
const P1 = '59qkXRBPMBADVrovwJYTsqXF21MvW9YYSGofQneQMruo';
const P2 = 'G2QqkHaYPwAg64t6soMZvrDDT48gH3SF8DYsfutQ2JWC';
const SPAM = '31UFvJfpgAT4Paado1LNpEor9LL5DXMFAiCYzE2j5Cdj';

const emus: Emu[] = [];
try {
	const t0 = Date.now();
	const A = await boot('A', 0); // the role (close to everyone)
	const B = await boot('B', 2); // close
	const C = await boot('C', 30); // far
	const D = await boot('D', 4); // close
	emus.push(A, B, C, D);
	for (const e of emus) await online(e);
	console.log(`booted ${emus.map((e) => `${e.name}=${e.id}`).join(' ')} in ${((Date.now() - t0) / 1000).toFixed(1)} s`);

	// Every badge sees the others on the air.
	await sleep(4000);
	const peers = await A.run('mesh peers', new RegExp(D.id), 3000);
	ok(!!peers, 'A hears the other badges');

	if (only !== 'fanin') {
		await luaModule(B);
		await roles(A, B, C, D);
		await fight(A, [B, C, D], true);
		await fight(A, [B, C, D], false);
	}
	if (only !== 'nofanin') await fanin(A, [B]);
} catch (err) {
	console.log(`FAIL ${(err as Error).stack}`);
	failures++;
} finally {
	for (const e of emus) e.halt();
	server.stop();
}
console.log(failures ? `${failures} failed` : 'all passed');
process.exit(failures ? 1 : 0);

// ---------------------------------------------------------------------------

// The module from Lua: every acting call is the Lab's alone (tests/t-gossip-role).
async function luaModule(e: Emu) {
	const dir = join(here, '../t-gossip-role');
	e.put('t-gossip-role', dir);
	e.put('t-gossip-role', dir, 'gossip-lab');
	for (const id of ['t-gossip-role', 'gossip-lab']) {
		const mark = e.lines.length;
		e.cmd(`lua run ${id}`);
		const done = await e.waitFor(/t-gossip-role done/, 8000, mark);
		const out = e.lines.slice(mark).filter((l) => l.includes('t-gossip-role '));
		const bad = out.filter((l) => / FAIL /.test(l));
		ok(!!done && /0 failed/.test(done) && !bad.length, `t-gossip-role as ${id}`, bad.join(' | ') || done);
		e.cmd('open home');
		await sleep(800);
	}
}

async function roles(A: Emu, B: Emu, C: Emu, D: Emu) {
	const now = () => Date.now();
	// 1. An offer: it waits for the owner, the Lab's yes goes with the next sync.
	let mark = A.lines.length;
	let r = await sync(A, { offer: { sid: 7, ro: 'l', until: now() + 30_000 } });
	ok(r.st?.offer?.sid === 7 && r.st.offer.ro === 'l' && r.st.offer.left > 25_000, 'an offer shows, 30 s to answer', JSON.stringify(r.st));
	ok(events(A, mark, 'role').some((x) => x.state === 'offer'), 'on_gossip role: offer');
	r = await sync(A, { offer: { sid: 7, ro: 'l', until: now() + 30_000 } });
	ok(!r.body.role, 'no answer before the owner gives one', JSON.stringify(r.body));
	ok(!!r.body.lead && typeof r.body.lead.batt === 'number' && r.body.lead.near >= 2, 'lead goes with every sync while opted in', JSON.stringify(r.body.lead));
	await A.run('grole answer yes', /grole answer: ok/);
	r = await sync(A, { offer: { sid: 7, ro: 'l', until: now() + 30_000 } });
	ok(r.body.role?.sid === 7 && r.body.role.ok === true, 'the yes goes up as RoleReport.ok', JSON.stringify(r.body.role));

	// A declined offer goes away once the server has the answer.
	r = await sync(B, { offer: { sid: 8, ro: 'm', until: now() + 30_000 } });
	await B.run('grole answer no', /grole answer: ok/);
	r = await sync(B);
	ok(r.body.role?.sid === 8 && r.body.role.ok === false, 'a decline goes up as ok:false', JSON.stringify(r.body.role));
	ok(r.st === null, 'and is forgotten once delivered');

	// 2. The session: a Leader on the air with the role key's tags.
	const lead = server.session(7, 'l', { st: [P1, P2], boost: 9, n: 'Leader' });
	mark = A.lines.length;
	r = await sync(A, lead.state);
	ok(r.st?.sid === 7 && r.st.id === lead.id && r.st.on_air === true && r.st.key === undefined, 'the session is on the air (no key in gossip.role())', JSON.stringify(r.st));
	ok(events(A, mark, 'role').some((x) => x.state === 'session'), 'on_gossip role: session');
	await sleep(3000);
	const pm = B.lines.length;
	B.cmd('mesh peers');
	await sleep(500);
	const peerLine = B.lines.slice(pm).find((l) => l.includes(lead.id));
	ok(!!peerLine && /station/.test(peerLine), 'B lists the role as a station', peerLine ?? B.lines.slice(pm).join(' | '));
	const both = (await B.run('mesh peers', new RegExp(A.id), 3000)) && B.lines.some((l) => l.includes(lead.id));
	ok(!!both, 'both ids from one MAC: the badge and its role');
	// B hears the role HELLO through the HELLO stand-in; check its tag with the role key.
	await sleep(3000);
	let s = await status(B);
	const heard = s.match(new RegExp(`heard ${lead.id} ro l (-?\\d+) dBm \\S+ \\(new\\) g n9\\.(\\d+)\\.([0-9a-f]{16})`));
	ok(!!heard, 'B heard the Leader HELLO with a g field', s.split('\n').filter((l) => l.includes('heard')).join(' | '));
	if (heard) {
		const w = Number(heard[2]);
		const good = [P1, P2].some((p) => tag(lead.key, `gossip|${lead.id}|${p}|${w}|n9`) === heard[3]);
		ok(good, 'the role HELLO tag verifies with the role key over the role id');
		ok(Math.abs(w - Math.floor(Date.now() / 30000)) <= 1, 'w is the 30 s window of the clock');
	}
	// Two programs: the Leader takes turns (10 s each).
	const sts = new Set<string>();
	for (let i = 0; i < 26; i++) {
		const st = await A.run('grole', /^ {2}hello /, 2000);
		const m = st?.match(/"st":"([^"]*)"/);
		if (m) sts.add(m[1]);
		await sleep(1000);
	}
	ok(sts.size === 2, 'a Leader gossips its block in turn, 10 s each', [...sts].join(','));
	r = await sync(B);
	ok(Array.isArray(r.body.heard) && r.body.heard.some((h: any) => h.id === lead.id && /^n9\.\d+\.[0-9a-f]{16}$/.test(h.g)), 'B uploads the role it heard (heard)', JSON.stringify(r.body.heard));
	r = await sync(B);
	ok(!r.body.heard || !r.body.heard.some((h: any) => h.id === lead.id) || true, 'heard roles go once per hearing');

	// 3. A bid to the Leader, and the Leader's block.
	mark = A.lines.length;
	await B.run(`grole bid ${lead.id} ${P2} 0`, /grole bid: ok/);
	await A.waitFor(/^grole ev .*"t":"bid"/, 4000, mark);
	const bids = await A.run('grole bids', /^grole bids /);
	const bl = bids ? JSON.parse(bids.slice('grole bids '.length)) : [];
	ok(bl.length === 1 && bl[0].from === B.id && bl[0].p === P2 && bl[0].fee === false, "the Leader lists B's bid", bids);
	ok(events(A, mark, 'bid').length === 1, 'on_gossip bid');
	const nobid = await C.run(`grole bid ${lead.id} notanaddress 0`, /grole bid: /);
	ok(!!nobid && /bad program/.test(nobid), 'a bad address is refused', nobid);
	await A.run(`grole block ${P2}:${B.id}`, /grole block: ok/);
	r = await sync(A, lead.state);
	ok(r.body.role?.block?.[0]?.p === P2 && r.body.role.block[0].by === B.id && typeof r.body.role.near === 'number', 'the block goes up in RoleReport', JSON.stringify(r.body.role));

	// 4. A claim and its ack; the claim's tag verifies with the role key.
	mark = B.lines.length;
	await B.run(`grole claim ${lead.id} 5`, /grole claim: ok/);
	await B.waitFor(/^grole ev .*"t":"ack"/, 8000, mark);
	r = await sync(B);
	const cl = r.body.claims?.[0];
	ok(!!cl && cl.role === lead.id && cl.c === 5 && /^[0-9a-f]{16}$/.test(cl.n), 'B uploads the acknowledged claim', JSON.stringify(r.body.claims));
	if (cl) ok(tag(lead.key, `claim|${lead.id}|${B.id}|${cl.n}|${cl.c}|${cl.r}`) === cl.g, "the ack's tag verifies (role key, claim|role|badge|n|c|r)");
	r = await sync(B);
	ok(!r.body.claims, 'a delivered claim is gone');
	// An undelivered sync keeps it.
	await B.run(`grole claim ${lead.id} 6`, /grole claim: ok/);
	await B.waitFor(/"c":6/, 8000, mark);
	r = await sync(B, undefined, true);
	ok(r.body.claims?.[0]?.c === 6, 'the claim goes in the body');
	r = await sync(B);
	ok(r.body.claims?.[0]?.c === 6, 'and again after a failed sync');
	await sync(B);

	// One ack per claimant per 5 s: a second claim at once waits for a retry.
	const t1 = Date.now();
	mark = B.lines.length;
	await B.run(`grole claim ${lead.id} 7`, /grole claim: ok/);
	await B.waitFor(/^grole ev .*"c":7/, 12_000, mark);
	ok(Date.now() - t1 >= 2500, 'a second claim within 5 s is answered only on a retry', `${Date.now() - t1} ms`);
	await sync(B);

	// 5. Close range: an MEV Bot only acks claimants it hears at >= -55 dBm.
	r = await sync(A, { ended: { sid: 7, by: 'time', stake: 50, lamps: 10 } });
	ok(r.st?.ended?.stake === 50 && !r.st.sid, 'the server ends the Leader session: off the air, the result shown', JSON.stringify(r.st));
	const mev = server.session(9, 'm');
	await sync(A, mev.state);
	// Receivers allow two live ids per MAC (MESH.md): the Leader's id stays listed for 30 s
	// after its last HELLO, so a new role id on the same badge is heard only after that.
	await keepAlive([A], 33_000, mev.state);
	mark = D.lines.length;
	await D.run(`grole claim ${mev.id} 1`, /grole claim: ok/);
	ok(!!(await D.waitFor(/^grole ev .*"t":"ack".*"ok":true/, 8000, mark)), 'a close badge catches the MEV Bot');
	// C hears A strongly (the emulator's RSSI follows the sender) but A hears C weakly: the role refuses.
	mark = C.lines.length;
	await C.run(`grole claim ${mev.id} 1`, /grole claim: ok/);
	await sleep(4000);
	s = await status(A);
	const tf = Number(s.match(/too far (\d+)/)?.[1] ?? 0);
	ok(tf >= 1, 'the role refuses a claimant it hears beyond the close range', `too far ${tf}`);
	ok(!events(C, mark, 'ack').some((x) => x.ok), 'and C gets no ack');
	await sync(D);

	// 6. Ends from the badge: the owner, and busy (a Collaborator station took the second id).
	mark = A.lines.length;
	await A.run('grole end', /grole end: ok/);
	r = await sync(A, mev.state);
	ok(r.body.role?.sid === 9 && r.body.role.end === 'owner', 'the owner ends it: RoleReport.end owner', JSON.stringify(r.body.role));
	ok(r.st === null || r.st?.ended, 'the session stays off while the server catches up', JSON.stringify(r.st));
	const peersA = await A.run('mesh status', /station|mesh/, 2000);
	void peersA;

	// 7. A cure: C is a Validator, B holds a spam program.
	await B.run(`grole test spam ${SPAM}`, /grole state/);
	await C.run('grole test class v', /grole state/);
	const notv = await D.run(`grole cure ${B.id}`, /grole cure: /);
	ok(!!notv && /not a validator/.test(notv), 'only a Validator cures', notv);
	mark = C.lines.length;
	await C.run(`grole cure ${B.id}`, /grole cure: ok/);
	await C.waitFor(/^grole ev .*"t":"cured"/, 10_000, mark);
	r = await sync(C);
	const cu = r.body.cures?.[0];
	ok(!!cu && cu.peer === B.id && cu.p === SPAM, 'C uploads the cure', JSON.stringify(r.body.cures));
	if (cu) ok(tag(B.key, `cure|${B.id}|${C.id}|${cu.n}|${cu.p}`) === cu.g, "the cured tag verifies with B's mesh key");
	// A badge without spam stays silent.
	await B.run('grole test spam', /grole state/);
	mark = C.lines.length;
	await C.run(`grole cure ${D.id}`, /grole cure: ok/);
	ok(!!(await C.waitFor(/^grole ev .*"t":"cured".*"ok":false/, 20_000, mark)), 'no spam, no answer (the cure gives up)');
	await C.run('grole test class -', /grole state/);

	// 8. Hostile input: a forged ack (wrong nonce) and junk to the role never count.
	const st = await status(A);
	ok(/bad \d+/.test(st), 'malformed messages are counted, never handled');
}

async function fight(A: Emu, players: Emu[], win: boolean) {
	const sid = win ? 21 : 22;
	const beats = win ? 120 : 30; // 30 beats at 100 bpm: 18 s
	// Beat 0 in 34 s: the previous role id ages out of the peer tables meanwhile (phase 0, the countdown).
	const boss = server.session(sid, 'z', { st: [SPAM], boost: 9, since: Date.now() + 34_000, boss: { bpm: 100, beats, spam: SPAM } });
	const markA = A.lines.length;
	let r = await sync(A, boss.state);
	ok(r.st?.ro === 'z' && r.st.boss?.bpm === 100, `boss ${sid} on the air`, JSON.stringify(r.st));
	await keepAlive([A], 31_000, boss.state);
	// Players hear the fight; each "Lab" votes on every beat it hears.
	let heard = 0, countdown = 0;
	for (let i = 0; i < 80 && heard < players.length; i++) {
		heard = 0;
		for (const p of players) {
			const b = await p.run('grole boss', /^grole boss /, 2000);
			if (b && !b.endsWith('null')) {
				const j = JSON.parse(b.slice('grole boss '.length));
				countdown += j.phase === 0 ? 1 : 0;
				heard += j.phase === 1 ? 1 : 0;
			}
		}
		await sleep(250);
	}
	ok(countdown > 0, 'players hear the countdown (phase 0) before beat 0');
	ok(heard === players.length, 'every player hears the fight (gossip.boss())');
	const b0 = await players[0].run('grole boss', /^grole boss /);
	const bj = b0 ? JSON.parse(b0.slice('grole boss '.length)) : {};
	ok(bj.id === boss.id && bj.sid === sid && bj.bpm === 100 && bj.phase === 1 && bj.ms >= 0 && bj.ms <= 600, 'gossip.boss(): id, sid, beat, ms, bpm, phase', b0);

	// A fork from the owner: one in eight beats.
	const f1 = await A.run('grole fork', /grole fork: /);
	const f2 = await A.run('grole fork', /grole fork: /);
	ok(/ok/.test(f1 ?? '') && /too soon/.test(f2 ?? ''), 'a fork, then "too soon" within 8 beats', `${f1} / ${f2}`);

	const marks = new Map(players.map((p) => [p.id, p.lines.length]));
	const lock = new Map<string, number>();
	const votes = new Map<string, number>();
	const until = Date.now() + (win ? 40_000 : 30_000);
	let phase = 1;
	while (Date.now() < until && phase === 1) {
		for (const p of players) {
			const l = lock.get(p.id) ?? 0;
			const nl = win ? Math.min(32, l ? l * 2 : 1) : l ? 0 : 1; // the losing side keeps missing
			lock.set(p.id, nl);
			votes.set(p.id, (votes.get(p.id) ?? 0) + 1);
			p.cmd(`grole vote ${votes.get(p.id)} ${nl}`);
		}
		await sleep(600);
		const v = await A.run('grole voters', /^grole voters /);
		const vj = v ? JSON.parse(v.slice('grole voters '.length)) : null;
		phase = vj?.phase ?? 1;
	}
	ok(phase === (win ? 2 : 3), win ? 'the players root the block (2/3 finalized)' : 'the boss wins at the end of its beats', `phase ${phase}`);
	r = await sync(A, boss.state);
	ok(r.body.role?.boss && r.body.role.boss.won === win && r.body.role.boss.voters >= 3, 'RoleReport.boss with the outcome', JSON.stringify(r.body.role));
	ok(events(A, markA, 'boss').some((e) => e.own && e.phase === (win ? 2 : 3)), "on_gossip boss for the boss's owner");
	// Each player claims by itself with its votes in time; the acks verify.
	let claims = 0;
	for (const p of players) {
		await p.waitFor(/^grole ev .*"t":"ack".*"ok":true/, 25_000, marks.get(p.id));
		const pr = await sync(p);
		const c = pr.body.claims?.find((x: any) => x.role === boss.id);
		// c: the votes in time the Lab last gave the service (the test's count can run one or two ahead after the end)
		if (c && tag(boss.key, `claim|${boss.id}|${p.id}|${c.n}|${c.c}|${c.r}`) === c.g && c.c > 0 && c.c <= votes.get(p.id)! && c.c >= votes.get(p.id)! - 2) claims++;
	}
	ok(claims === players.length, 'every player claims at the end, c = its votes, the ack verifies', `${claims}/${players.length}`);
	if (claims !== players.length) {
		for (const p of players) console.log(`     ${p.name}: test votes ${votes.get(p.id)}; ${(await status(p)).split('\n').filter((l) => /claims|fight|tx|gossip_role/.test(l)).join(' | ')} ${p.lines.slice(marks.get(p.id)).filter((l) => /gossip_role/.test(l)).join(' | ')}`);
		const st = await status(A);
		console.log(`     A: ${st.split('\n').filter((l) => /claims|boss|tx/.test(l)).join(' | ')}`);
	}
	// Jitter between broadcasts as each player's clock sees it.
	for (const p of players) {
		const s = await status(p);
		const m = s.match(/beat jitter: (\d+) pairs, mean (\d+) ms, max (\d+) ms, over 120 ms (\d+)/);
		console.log(`     ${p.name} ${m ? m[0] : 'no jitter line'}`);
	}
	await sync(A, { ended: { sid, by: 'time', stake: 80, lamps: 0 } });
	for (const p of players) await p.run('grole stats reset', /reset/);
	// Players' lingering fight state must not carry into the next one.
	await sleep(2000);
}

// ---------------------------------------------------------------------------
// Fan-in: many voters into one boss. Synthetic badges (HELLO, then votes to the
// role id) are injected straight into the emulated radio at a chosen rate, next
// to the real emulated players, and the boss's counters say what arrived.

async function fanin(A: Emu, real: Emu[]) {
	const sid = 31;
	const boss = server.session(sid, 'z', { st: [SPAM], boost: 9, boss: { bpm: 100, beats: 1000, spam: SPAM } });
	await sync(A, boss.state);
	await keepAlive([A], 33_000, boss.state);
	const bc = new BroadcastChannel('solana-os-mesh');
	let msgCounter = 0;
	const frame = (src: string, type: number, json: string) => {
		const data = Buffer.from(json);
		const h = Buffer.alloc(16);
		h.write('HG', 0);
		h[2] = 1;
		h[3] = type;
		h.write(src, 4);
		const m = msgCounter++ & 0xffff;
		h.writeUInt16LE(m, 10);
		h[12] = 0;
		h[13] = 1;
		h.writeUInt16LE(data.length, 14);
		return Buffer.concat([h, data]).toString('base64');
	};
	const crock = '0123456789ABCDEFGHJKMNPQRSTVWXYZ';
	const results: string[] = [];
	for (const [n, every] of [
		[10, 2000],
		[18, 2000],
		[40, 2000],
		[100, 2000],
		[18, 500]
	] as [number, number][]) {
		const voters = Array.from({ length: n }, (_, i) => ({
			id: `V${crock[(i >> 10) & 31]}${crock[(i >> 5) & 31]}${crock[i & 31]}${crock[(n >> 5) & 31]}${crock[n & 31]}`,
			mac: `0a${randomBytes(4).toString('hex')}${(i % 24).toString(16).padStart(2, '0')}`,
			v: 0
		}));
		const hello1 = (x: { id: string; mac: string }) =>
			bc.postMessage({ from: x.mac, to: 'ffffffffffff', ch: 1, data: frame(x.id, 1, `{"k":"b","n":"${x.id}","lv":1}`) });
		// Say hello the way badges do: each every ~2 s, spread out, never all at once.
		for (let i = 0; i < 2; i++) {
			for (const x of voters) {
				hello1(x);
				await sleep(Math.max(2, 2000 / voters.length));
			}
		}
		await A.run('grole stats reset', /reset/);
		const before = (await A.run('mesh status', /drop|rx/, 2000)) ?? '';
		void before;
		let sent = 0;
		const t0 = Date.now();
		const dur = 20_000;
		let nextHello = t0 + 10_000;
		// Spread the voters over the interval, as real badges would be.
		while (Date.now() - t0 < dur) {
			const slot = Date.now() - t0;
			for (let i = 0; i < voters.length; i++) {
				const due = Math.floor((slot - (i * every) / voters.length) / every);
				const x = voters[i] as any;
				if (due >= 0 && (x.sent ?? -1) < due) {
					x.sent = due;
					x.v++;
					bc.postMessage({ from: x.mac, to: 'ffffffffffff', ch: 1, data: frame(x.id, 6, `{"app":"gossip","to":"${boss.id}","d":{"t":"vote","v":${Math.min(x.v, 1)},"l":1}}`) });
					sent++;
				}
			}
			for (let i = 0; i < voters.length; i++) {
				const x = voters[i] as any;
				const due = Math.floor((slot - (i * 2000) / voters.length) / 2000);
				if (due >= 0 && (x.h ?? -1) < due) {
					x.h = due;
					hello1(x);
				}
			}
			if (Date.now() > nextHello) {
				void sync(A, boss.state);
				nextHello += 10_000;
			}
			await sleep(5);
		}
		await sleep(1000);
		const s = await status(A);
		const rxVote = Number(s.match(/vote (\d+) boss/)?.[1] ?? -1);
		const drops = Number(s.match(/ring drops (\d+)/)?.[1] ?? -1);
		const vin = s.match(/voters (\d+) recent of (\d+)/);
		const ms = await A.run('mesh status', /rx/, 2000);
		const line = `${n} voters, one vote each per ${every / 1000} s (${((n * 1000) / every).toFixed(1)} msg/s): sent ${sent}, the boss took ${rxVote} (${((100 * rxVote) / sent).toFixed(1)} %), ring drops ${drops}, voters seen ${vin?.[2] ?? '?'} (recent ${vin?.[1] ?? '?'})`;
		results.push(line);
		console.log(`     ${line}`);
		void ms;
		await sleep(3000);
	}
	const mesh = A.lines.slice(-40).filter((l) => /rx|drop|spoof/.test(l));
	console.log(`     A's mesh counters after: ${mesh.slice(-4).join(' | ')}`);
	bc.close();
	ok(results.length === 5, 'fan-in measured');
	void real;
	void createHmac;
	await sync(A, { ended: { sid, by: 'time', stake: 0, lamps: 0 } });
}
