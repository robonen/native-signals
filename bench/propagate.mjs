// alien-signals reference run — identical graph shapes, op and measurement
// protocol as bench/propagate.cpp, so the two outputs can be compared directly.
//
//   node --expose-gc bench/propagate.mjs /path/to/alien-signals/esm/index.mjs
const modPath = process.argv.slice(2).find(a => a.endsWith('.js') || a.endsWith('.mjs')) ?? 'alien-signals';
const { computed, effect, signal } = await import(modPath);

const results = [];

function bench(name, iters, setup) {
	const op = setup();
	for (let i = 0; i < iters / 10 + 1; i++) op();  // warmup
	const t0 = process.hrtime.bigint();
	for (let i = 0; i < iters; i++) op();
	const t1 = process.hrtime.bigint();
	const ns = Number(t1 - t0) / iters;
	results.push({ name, ns });
	console.log(name.padEnd(34) + ns.toFixed(1).padStart(10) + ' ns/op');
}

let sink = 0;

function propagate(w, h, iters) {
	bench(`propagate ${w}x${h}`, iters, () => {
		const src = signal(1);
		const keepalive = [];
		for (let i = 0; i < w; i++) {
			let last = computed(() => src());
			for (let j = 0; j < h; j++) {
				const prev = last;
				last = computed(() => prev() + 1);
				keepalive.push(last);
			}
			const tail = last;
			keepalive.push(effect(() => { sink = tail(); }));
		}
		return () => src(src() + 1);
	});
}

function broadcast(n, iters) {
	bench(`broadcast 1->${n}`, iters, () => {
		const src = signal(0);
		const keepalive = [];
		for (let i = 0; i < n; i++) keepalive.push(effect(() => { sink = src(); }));
		return () => src(src() + 1);
	});
}

function deepUnchanged(depth, iters) {
	bench(`deep-unchanged d=${depth}`, iters, () => {
		const src = signal(0);
		const keepalive = [];
		let last = computed(() => src() & 0);
		keepalive.push(last);
		for (let i = 0; i < depth; i++) {
			const prev = last;
			last = computed(() => prev());
			keepalive.push(last);
		}
		const tail = last;
		keepalive.push(effect(() => { sink = tail(); }));
		return () => src(src() + 1);
	});
}

function cachedRead(iters) {
	bench('cached computed read', iters, () => {
		const a = signal(1);
		const c = computed(() => a() * 2);
		sink = c();
		return () => { sink = c(); };
	});
}

function createDestroy(iters) {
	bench('create+destroy triple', iters, () => () => {
		const a = signal(1);
		const c = computed(() => a() * 2);
		const stop = effect(() => { sink = c(); });
		stop();
	});
}

function unstableDeps(iters) {
	bench('unstable deps (16 sources)', iters, () => {
		const sel = signal(0);
		const sources = Array.from({ length: 16 }, (_, i) => signal(i));
		const stop = effect(() => { sink = sources[sel() % 16](); });
		let i = 0;
		return () => sel(++i);
	});
}

function memoryReport() {
	const n = 10000;
	console.log(`\n-- memory (heapUsed, ${n} nodes each) --`);
	globalThis.gc();
	let start = process.memoryUsage().heapUsed;

	const signals = Array.from({ length: n }, () => signal(0));
	globalThis.gc();
	let end = process.memoryUsage().heapUsed;
	console.log(`signal:   ${((end - start) / 1024).toFixed(2).padStart(8)} KB (${((end - start) / n).toFixed(1)} B/node)`);

	start = end;
	const computeds = Array.from({ length: n }, (_, i) => computed(() => signals[i]() + 1));
	globalThis.gc();
	end = process.memoryUsage().heapUsed;
	console.log(`computed: ${((end - start) / 1024).toFixed(2).padStart(8)} KB (${((end - start) / n).toFixed(1)} B/node)`);

	start = end;
	const effects = Array.from({ length: n }, (_, i) => effect(() => { sink = computeds[i](); }));
	globalThis.gc();
	end = process.memoryUsage().heapUsed;
	console.log(`effect:   ${((end - start) / 1024).toFixed(2).padStart(8)} KB (${((end - start) / n).toFixed(1)} B/node)`);
	if (effects.length !== n) throw new Error('unreachable');
}

const memOnly = process.argv.includes('--memory');
if (!memOnly) {
	console.log('-- alien-signals (Node ' + process.versions.node + ', full JIT) --');
	propagate(1, 1, 2_000_000);
	propagate(10, 10, 200_000);
	propagate(100, 100, 3_000);
	propagate(1000, 10, 3_000);
	broadcast(1000, 20_000);
	deepUnchanged(100, 200_000);
	deepUnchanged(1000, 20_000);
	cachedRead(20_000_000);
	createDestroy(2_000_000);
	unstableDeps(2_000_000);
}
memoryReport();
