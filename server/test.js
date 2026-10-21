// Exercises the relay with two clients: node test.js
'use strict';

const assert = require('assert');
const WebSocket = require('ws');
const { createRelay } = require('./relay');

function connect(port) {
	return new Promise((resolve, reject) => {
		const ws = new WebSocket(`ws://localhost:${port}`);
		ws.inbox = [];
		ws.waiters = [];
		ws.on('message', (data, isBinary) => {
			const msg = isBinary ? { binary: Buffer.from(data) } : JSON.parse(data.toString());
			const waiter = ws.waiters.shift();
			if (waiter) waiter(msg);
			else ws.inbox.push(msg);
		});
		ws.on('open', () => resolve(ws));
		ws.on('error', reject);
	});
}

function next(ws) {
	if (ws.inbox.length) return Promise.resolve(ws.inbox.shift());
	return new Promise((resolve, reject) => {
		const timer = setTimeout(() => reject(new Error('timed out waiting for a message')), 2000);
		ws.waiters.push(msg => { clearTimeout(timer); resolve(msg); });
	});
}

const sendJson = (ws, msg) => ws.send(JSON.stringify(msg));

async function main() {
	const relay = await createRelay({ port: 0, log: () => {}, graceMs: 500 });
	try {
		const a = await connect(relay.port);
		const b = await connect(relay.port);

		sendJson(b, { t: 'join', code: 'ZZZZ' });
		assert.deepStrictEqual(await next(b), { t: 'error', msg: 'No room with that code' });

		sendJson(a, { t: 'create', map: 2 });
		const created = await next(a);
		assert.strictEqual(created.t, 'created');
		assert.match(created.code, /^[A-Z]{4}$/);
		assert.match(created.token, /^[0-9a-f]{32}$/);

		sendJson(b, { t: 'join', code: created.code.toLowerCase() });
		const joined = await next(b);
		assert.strictEqual(joined.t, 'joined');
		assert.strictEqual(joined.code, created.code);
		assert.strictEqual(joined.map, 2);
		assert.notStrictEqual(joined.token, created.token);
		assert.deepStrictEqual(await next(a), { t: 'opponent_joined' });
		const startA = await next(a);
		const startB = await next(b);
		assert.strictEqual(startA.t, 'start');
		assert.strictEqual(startA.seed, startB.seed);
		assert.strictEqual(startA.map, 2);
		assert.strictEqual(startB.map, 2);

		const c = await connect(relay.port);
		sendJson(c, { t: 'join', code: created.code });
		assert.deepStrictEqual(await next(c), { t: 'error', msg: 'That room is full' });
		sendJson(c, { t: 'rejoin', code: created.code, token: 'nope' });
		assert.deepStrictEqual(await next(c), { t: 'error', msg: 'That match has ended' });
		c.close();

		sendJson(a, { t: 'ping' });
		assert.deepStrictEqual(await next(a), { t: 'pong' });

		sendJson(a, { t: 'ready' });
		assert.deepStrictEqual(await next(b), { t: 'opponent_ready' });
		sendJson(b, { t: 'ready' });
		assert.deepStrictEqual(await next(a), { t: 'opponent_ready' });
		assert.deepStrictEqual(await next(a), { t: 'go' });
		assert.deepStrictEqual(await next(b), { t: 'go' });

		a.send(Buffer.from([1, 2, 3, 250]));
		assert.deepStrictEqual(await next(b), { binary: Buffer.from([1, 2, 3, 250]) });
		b.send(Buffer.from([9]));
		assert.deepStrictEqual(await next(a), { binary: Buffer.from([9]) });

		// B drops out; what A sends meanwhile reaches B when B rejoins
		b.terminate();
		assert.deepStrictEqual(await next(a), { t: 'opponent_disconnected' });
		a.send(Buffer.from([1, 7, 2]));
		const b2 = await connect(relay.port);
		sendJson(b2, { t: 'rejoin', code: created.code, token: joined.token });
		assert.deepStrictEqual(await next(b2), { t: 'rejoined', code: created.code, go: true, opponentConnected: true, opponentReady: true });
		assert.deepStrictEqual(await next(b2), { binary: Buffer.from([1, 7, 2]) });
		assert.deepStrictEqual(await next(a), { t: 'opponent_reconnected' });
		a.send(Buffer.from([4]));
		assert.deepStrictEqual(await next(b2), { binary: Buffer.from([4]) });

		// B drops out again and doesn't come back in time: A wins
		b2.terminate();
		assert.deepStrictEqual(await next(a), { t: 'opponent_disconnected' });
		assert.deepStrictEqual(await next(a), { t: 'opponent_left' });
		const b3 = await connect(relay.port);
		sendJson(b3, { t: 'rejoin', code: created.code, token: joined.token });
		assert.deepStrictEqual(await next(b3), { t: 'error', msg: 'That match has ended' });
		b3.close();

		// Leaving on purpose forfeits straight away
		sendJson(a, { t: 'create' });
		const room2 = await next(a);
		const d = await connect(relay.port);
		sendJson(d, { t: 'join', code: room2.code });
		await next(d); await next(d); // joined, start
		await next(a); await next(a); // opponent_joined, start
		sendJson(d, { t: 'leave' });
		assert.deepStrictEqual(await next(a), { t: 'opponent_left' });
		d.close();
		a.close();

		console.log('relay tests passed');
	} finally {
		await relay.close();
	}
}

main().catch(err => {
	console.error(err);
	process.exit(1);
});
