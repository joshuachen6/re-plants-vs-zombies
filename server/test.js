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
	const relay = await createRelay({ port: 0 });
	try {
		const a = await connect(relay.port);
		const b = await connect(relay.port);

		sendJson(b, { t: 'join', code: 'ZZZZ' });
		assert.deepStrictEqual(await next(b), { t: 'error', msg: 'No room with that code' });

		sendJson(a, { t: 'create' });
		const created = await next(a);
		assert.strictEqual(created.t, 'created');
		assert.match(created.code, /^[A-Z]{4}$/);

		sendJson(b, { t: 'join', code: created.code.toLowerCase() });
		assert.deepStrictEqual(await next(b), { t: 'joined', code: created.code });
		assert.deepStrictEqual(await next(a), { t: 'opponent_joined' });
		const startA = await next(a);
		const startB = await next(b);
		assert.strictEqual(startA.t, 'start');
		assert.strictEqual(startA.seed, startB.seed);

		const c = await connect(relay.port);
		sendJson(c, { t: 'join', code: created.code });
		assert.deepStrictEqual(await next(c), { t: 'error', msg: 'That room is full' });
		c.close();

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

		b.close();
		assert.deepStrictEqual(await next(a), { t: 'opponent_left' });
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
