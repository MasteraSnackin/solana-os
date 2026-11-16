// Gossip end to end on two emulated badges against the fake server (fake.ts),
// headless, on the emulator's radio (a BroadcastChannel shared by both):
//   bun firmware/main/lua/tests/gossip/run.ts
// Loads web/static/emulator/solana-os.js (bun run emulator). Badge A runs the
// test app as the Lab (`gossip-lab`), picks a class and deploys a program; its
// HELLO then carries `st` and `g`. Badge B hears it, gets exposed, syncs the
// record, is adopted, receives the signed record and checks it. Then the reads
// on B (t-gossip, not the Lab: its acting calls say "lab only"), the radar, the
// service's switch, and A going quiet when gossip is off. Exit code 1 when
// anything is off. PORT (default 3211) for the fake.
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { startFake } from './fake';

const here = import.meta.dir;
const emuDir = join(here, '../../../../../web/static/emulator');
const fake = startFake(Number(process.env.PORT ?? 3211), { thr: 60, next: 4000 });
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

type Emu = {
	name: string;
	lines: string[];
	mod: any;
	cmd: (l: string) => void;
	since: (mark: number) => string[];
	waitFor: (re: RegExp, ms: number, mark?: number) => Promise<string | undefined>;
	put: (id: string, dir: string, as?: string) => void;
};

async function boot(name: string): Promise<Emu> {
	const lines: string[] = [];
	const nvs = new Map<string, string>();
	const url = new URL(fake.url);
	const loc = { origin: url.origin, href: `${url.origin}/emulator`, protocol: url.protocol, host: url.host, hostname: url.hostname, pathname: '/emulator', search: '' };
	const ls = {
		getItem: (k: string) => (nvs.has(k) ? nvs.get(k)! : null),
		setItem: (k: string, v: string) => void nvs.set(k, String(v)),
		removeItem: (k: string) => void nvs.delete(k),
		clear: () => nvs.clear(),
		key: (i: number) => [...nvs.keys()][i] ?? null,
		get length() { return nvs.size; },
	};
	const SolanaOS = new Function('window', 'location', 'localStorage', `${readFileSync(join(emuDir, 'solana-os.js'), 'utf8')}; return SolanaOS;`)(globalThis, loc, ls);
	const mod = await SolanaOS({
		locateFile: (p: string) => `file://${join(emuDir, p)}`,
		print: (l: string) => void lines.push(l),
		printErr: (l: string) => void lines.push(l),
		server: fake.url,
	});
	const since = (mark: number) => lines.slice(mark);
	const waitFor = async (re: RegExp, ms: number, mark = 0) => {
		for (let t = 0; t < ms; t += 50) {
			const hit = since(mark).find((l) => re.test(l));
			if (hit) return hit;
			await sleep(50);
		}
		return undefined;
	};
	const mkdirp = (p: string) => {
		let cur = '';
		for (const part of p.split('/').filter(Boolean)) {
			cur += '/' + part;
			try { mod.FS.mkdir(cur); } catch { /* exists */ }
		}
	};
	const put = (id: string, dir: string, as = id) => {
		const dest = `/sdcard/hh/apps/${as}`;
		mkdirp(dest);
		for (const f of readdirSync(dir)) {
			if (statSync(join(dir, f)).isFile()) {
				let data = readFileSync(join(dir, f), 'utf8');
				if (f === 'app.json') data = data.replace(`"${id}"`, `"${as}"`);
				mod.FS.writeFile(`${dest}/${f}`, data);
			}
		}
	};
	const e: Emu = { name, lines, mod, cmd: (l) => mod.ccall('emu_console', null, ['string'], [l]), since, waitFor, put };
	if (!(await waitFor(/console ready/, 20000))) throw new Error(`${name}: no console`);
	return e;
}

let failures = 0;
const ok = (cond: boolean, what: string) => {
	console.log(`${cond ? 'ok  ' : 'FAIL'} ${what}`);
	if (!cond) failures++;
};
const status = async (e: Emu) => {
	const mark = e.lines.length;
	e.cmd('gossip status');
	await e.waitFor(/^ {2}heard:/, 3000, mark);
	return e.since(mark).join('\n');
};
const until = async (e: Emu, test: (s: string) => boolean, ms: number) => {
	for (let t = 0; t < ms; t += 1000) {
		const s = await status(e);
		if (test(s)) return s;
		await sleep(1000);
	}
	return status(e);
};

const a = await boot('A');
const b = await boot('B');
for (const e of [a, b]) {
	e.cmd('wifi connect "Emulator Guest"');
}
for (const e of [a, b]) {
	const s = await until(e, (x) => /, claimed,/.test(x) && /clock yes/.test(x), 40000);
	ok(/, claimed,/.test(s) && /clock yes/.test(s), `${e.name}: claimed, synced, has the server's clock`);
}
const ids = [...fake.badges.values()].map((x) => x.id);
ok(ids.length === 2 && ids.every((id) => id.startsWith('e')), `two emulated badges registered: ${ids.join(', ')}`);

// 1. A acts as the Lab: a class, then its first program.
a.put('t-gossip', join(here, '../t-gossip'), 'gossip-lab');
let mark = a.lines.length;
a.cmd('lua run gossip-lab');
ok(!!(await a.waitFor(/t-gossip genome vector 3: ok/, 8000, mark)), 'A: gossip.genome decodes vector 3 (three traits)');
ok(!!(await a.waitFor(/t-gossip set_class: true/, 4000, mark)), 'A (the Lab): set_class queued');
ok(!!(await a.waitFor(/t-gossip deploy: true/, 4000, mark)), 'A (the Lab): deploy queued');
const res = await a.waitFor(/t-gossip ev: (?=.*"t":"result")(?=.*"act":"deploy")(?=.*"ok":true)/, 20000, mark);
ok(!!res, `A: the deploy's result came back as on_gossip ${res ? '' : '(none)'}`);
ok(!!(await a.waitFor(/t-gossip ev: .*"t":"node"/, 5000, mark)), 'A: on_gossip node after the sync');
ok(!!(await a.waitFor(/t-gossip ev: (?=.*"t":"event")(?=.*Deployed)/, 5000, mark)), 'A: on_gossip event (the server\'s ev)');
if (process.env.DEBUG) console.log(a.since(mark).filter((l) => /t-gossip|gossip|lua/.test(l)).join('\n'));
const prog = [...fake.records.keys()][0];
ok(!!prog, `the fake made program ${prog}`);
let sa = await until(a, (s) => s.includes(`"st":"${prog}"`), 15000);
const hello = /hello: (.*)/.exec(sa)?.[1] ?? '';
ok(new RegExp(`^"st":"${prog}","g":"r0\\.\\d+\\.[0-9a-f]{16}"$`).test(hello), `A's HELLO carries st and g: ${hello}`);
ok(hello.length + '{"k":"b","n":"Tester eXXXXX","lv":0,}'.length <= 232, `A's HELLO fits one fragment (~${hello.length + 37} bytes)`);
ok(/class r/.test(sa), 'A: class r from the node');
a.cmd('key home');

// 2. B hears A, is exposed, syncs, is adopted and checks the record.
mark = b.lines.length;
const sb = await until(b, (s) => new RegExp(`${prog} (exposed|incubating)`).test(s) || s.includes(`act ${prog}`), 60000);
ok(new RegExp(`${prog} (hearing|exposed|incubating)`).test(sb) || sb.includes(`act ${prog}`), 'B: hears A\'s program and gets exposed');
ok(!!(await b.waitFor(/gossip: exposed to/, 60000, 0)), 'B: the exposure logged');
const adopted = await until(b, (s) => s.includes(`act ${prog}`), 30000);
ok(adopted.includes(`act ${prog}`), 'B: the fake adopted it; B\'s node holds it');
const bsync = fake.syncs.find((x) => x.id !== fake.syncs[0]?.id && x.body.rec?.length);
const rec = fake.syncs.flatMap((x) => x.body.rec ?? []).find((r: any) => r.p === prog);
ok(!!rec && rec.f && /^r0\.\d+\.[0-9a-f]{16}$/.test(rec.g) && rec.s > 0 && rec.r < 0 && rec.t > 1.7e9, `B's exposure record: ${JSON.stringify(rec)}`);
void bsync;
let progs = '';
for (let t = 0; t < 20 && !progs.includes('verified'); t++) {
	mark = b.lines.length;
	b.cmd('gossip progs');
	await b.waitFor(/gossip progs:/, 3000, mark);
	await sleep(300);
	progs = b.since(mark).join('\n');
	if (!progs.includes(`${prog} verified`)) await sleep(1000);
}
ok(progs.includes(`${prog} verified`), 'B: the record\'s PDA and signature check (the checker task)');

// 3. The reads on B (not the Lab).
b.put('t-gossip', join(here, '../t-gossip'));
mark = b.lines.length;
b.cmd('lua run t-gossip');
const me = await b.waitFor(/t-gossip me: /, 8000, mark);
ok(!!me && me.includes(`"act":"${prog}"`) && me.includes('"on":true') && /"synced":\d{13}/.test(me), `B: gossip.me() ${me?.slice(0, 160)}`);
const pr = await b.waitFor(/t-gossip program: /, 4000, mark);
ok(!!pr && pr.includes('"verified":true') && pr.includes(`"a":"${prog}"`) && /"traits":\[/.test(pr) && /"nodes":2/.test(pr), `B: gossip.program(act) ${pr?.slice(0, 200)}`);
const ps = await b.waitFor(/t-gossip programs: /, 4000, mark);
ok(!!ps && ps.includes('"slots":[{') && ps.includes('false'), `B: gossip.programs() ${ps?.slice(0, 120)}`);
const nb = await b.waitFor(/t-gossip nearby: /, 4000, mark);
const aId = ids.find((id) => b.lines.join('\n').includes(id) && id !== /, claimed/.exec('')?.[0]) ?? '';
ok(!!nb && nb.includes(`"st":"${prog}"`) && nb.includes('"class":"r"') && nb.includes('"kind":"badge"'), `B: gossip.nearby() lists A ${nb?.slice(0, 200)}`);
ok(!!(await b.waitFor(/t-gossip set_class: nil lab only/, 4000, mark)), 'B: acting outside the Lab: nil, "lab only"');
void aId;
b.cmd('key home');

// 4. Off: A's HELLO loses its fields, one last sync says on: false.
const before = fake.syncs.length;
a.cmd('gossip off');
sa = await until(a, (s) => s.includes('(no gossip fields)'), 8000);
ok(sa.includes('(no gossip fields)') && /gossip: off \(Off\)/.test(sa), 'A off: no st/g in the HELLO, status Off');
await sleep(1500);
ok(fake.syncs.slice(before).some((x) => x.body.on === false), 'A off: a last sync with on: false');
mark = a.lines.length;
a.cmd('svc');
await a.waitFor(/services, \d+ running/, 3000, mark);
ok(a.since(mark).some((l) => /^gossip .*stopped|^gossip .*Off/i.test(l)), `Services lists gossip as stopped: ${a.since(mark).find((l) => l.startsWith('gossip')) ?? '(no line)'}`);
a.cmd('gossip on');
sa = await until(a, (s) => s.includes(`"st":"${prog}"`), 15000);
ok(sa.includes(`"st":"${prog}"`), 'A on again: st and g back in the HELLO');

console.log(failures ? `${failures} FAILED` : 'all passed');
for (const e of [a, b]) {
	try { e.mod.ccall('emu_halt', null, [], []); } catch { /* old build */ }
}
fake.stop();
process.exit(failures ? 1 : 0);
