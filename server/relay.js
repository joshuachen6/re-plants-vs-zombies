#!/usr/bin/env node
// Relay server for the Versus mode of the web build.
//
// Two players meet in a room identified by a 4-letter code. The server understands only the lobby
// messages (JSON text frames); everything else (binary frames) is game data that it forwards
// untouched to the other player in the room.
//
//   client -> server                          server -> client
//     {"t":"create","map":2}                     {"t":"created","code":"ABCD","token":"..."}
//     {"t":"join","code":"ABCD"}                 {"t":"joined","code":"ABCD","token":"...","map":2}
//     {"t":"rejoin","code":"ABCD","token":"..."} {"t":"rejoined","code":"ABCD","go":true,"opponentConnected":true,"opponentReady":true}
//     {"t":"ready"}                              {"t":"opponent_joined"}
//     {"t":"leave"}                              {"t":"start","seed":123,"map":2}
//     {"t":"ping"}                               {"t":"pong"}
//                                                {"t":"opponent_ready"}
//                                                {"t":"go"}
//                                                {"t":"opponent_disconnected"}  (they have a while to rejoin)
//                                                {"t":"opponent_reconnected"}
//                                                {"t":"opponent_left"}          (left, or didn't make it back)
//                                                {"t":"error","msg":"..."}
//
// A player whose connection drops mid-match keeps their spot for RECONNECT_GRACE_MS. Game data sent
// to them meanwhile is queued and delivered when they rejoin with the token they were given.
//
// The room's creator picks the map (0 day, 1 night, 2 pool, 3 fog, 4 roof); the server only passes
// it on.
//
// Usage: node relay.js [--port 8787] [--host 127.0.0.1] [--static ../build-web]

'use strict';

const crypto = require('crypto');
const http = require('http');
const fs = require('fs');
const path = require('path');
const { WebSocketServer } = require('ws');

// Clients are pinged this often and dropped after missing MAX_MISSED_PONGS replies in a row, so a
// short network stall doesn't end a match
const HEARTBEAT_MS = 15000;
const MAX_MISSED_PONGS = 2;
// A player who drops out of a match gets this long to reconnect before the opponent wins
const RECONNECT_GRACE_MS = 60000;
// Games send a keepalive every 2 s during a match; one that goes quiet this long has lost its
// connection even if TCP hasn't noticed, so its opponent is told (and paused) straight away
const MATCH_SILENCE_MS = 20000;
const MAX_QUEUED_FRAMES = 2000;
const NUM_MAPS = 5;
const CODE_LETTERS = 'ABCDEFGHJKLMNPQRSTUVWXYZ'; // no I or O, they read like 1 and 0

const MIME_TYPES = {
	'.html': 'text/html; charset=utf-8',
	'.js': 'text/javascript',
	'.wasm': 'application/wasm',
	'.data': 'application/octet-stream',
	'.png': 'image/png',
	'.ico': 'image/x-icon',
};

function parseArgs(argv) {
	const args = { port: Number(process.env.PORT) || 8787, host: undefined, staticDir: null };
	for (let i = 0; i < argv.length; i++) {
		if (argv[i] === '--port') args.port = Number(argv[++i]);
		else if (argv[i] === '--host') args.host = argv[++i];
		else if (argv[i] === '--static') args.staticDir = path.resolve(argv[++i]);
	}
	return args;
}

function serveStatic(staticDir, req, res) {
	if (!staticDir) {
		res.writeHead(200, { 'Content-Type': 'text/plain' });
		res.end('PvZ versus relay\n');
		return;
	}

	let urlPath = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
	if (urlPath === '/') urlPath = '/re-plants-vs-zombies.html';
	const filePath = path.join(staticDir, path.normalize(urlPath));
	if (!filePath.startsWith(staticDir + path.sep)) {
		res.writeHead(403);
		res.end();
		return;
	}

	fs.readFile(filePath, (err, data) => {
		if (err) {
			res.writeHead(404);
			res.end('Not found');
			return;
		}
		res.writeHead(200, { 'Content-Type': MIME_TYPES[path.extname(filePath)] || 'application/octet-stream' });
		res.end(data);
	});
}

function createRelay({ port, host, staticDir, log = console.log, graceMs = RECONNECT_GRACE_MS }) {
	let nextClientId = 1;

	// code -> room. A room has up to two player slots; a slot outlives its socket so a player who
	// drops out can come back with their token.
	//   room = { code, slots: [slot, slot?], ready: Set<slot>, started, seed, map }
	//   slot = { token, ws (null while disconnected), queue: [binary frames for it], graceTimer }
	const rooms = new Map();

	function send(ws, msg) {
		if (ws && ws.readyState === ws.OPEN) ws.send(JSON.stringify(msg));
	}

	function sendToSlot(slot, msg) {
		if (slot) send(slot.ws, msg);
	}

	function otherSlot(room, slot) {
		return room.slots.find(s => s !== slot);
	}

	function newCode() {
		for (;;) {
			let code = '';
			for (let i = 0; i < 4; i++) code += CODE_LETTERS[Math.floor(Math.random() * CODE_LETTERS.length)];
			if (!rooms.has(code)) return code;
		}
	}

	function newSlot(ws) {
		const slot = { token: crypto.randomBytes(16).toString('hex'), ws, queue: [], graceTimer: null };
		ws.slot = slot;
		return slot;
	}

	function closeRoom(room, reason) {
		for (const slot of room.slots) {
			clearTimeout(slot.graceTimer);
			if (slot.ws) {
				slot.ws.room = null;
				slot.ws.slot = null;
			}
		}
		rooms.delete(room.code);
		log(`room ${room.code} closed (${reason})`);
	}

	// The player left on purpose, or their spot ran out of time: the opponent wins
	function forfeit(room, slot, reason) {
		sendToSlot(otherSlot(room, slot), { t: 'opponent_left' });
		closeRoom(room, reason);
	}

	function socketClosed(ws) {
		const room = ws.room && rooms.get(ws.room);
		const slot = ws.slot;
		if (!room || !slot || slot.ws !== ws) return;

		// Nobody to play against yet, so there's nothing to come back to
		if (room.slots.length < 2) {
			closeRoom(room, 'host left the lobby');
			return;
		}

		slot.ws = null;
		sendToSlot(otherSlot(room, slot), { t: 'opponent_disconnected' });
		slot.graceTimer = setTimeout(() => forfeit(room, slot, 'player did not reconnect'), graceMs);
	}

	function leaveCurrentRoom(ws) {
		const room = ws.room && rooms.get(ws.room);
		if (room && ws.slot) forfeit(room, ws.slot, 'player left');
	}

	function handleControl(ws, msg) {
		const room = ws.room && rooms.get(ws.room);

		switch (msg.t) {
			case 'create': {
				leaveCurrentRoom(ws);
				const code = newCode();
				const slot = newSlot(ws);
				const map = Number.isInteger(msg.map) && msg.map >= 0 && msg.map < NUM_MAPS ? msg.map : 0;
				rooms.set(code, { code, slots: [slot], ready: new Set(), started: false, seed: 0, map });
				ws.room = code;
				log(`client ${ws.id} created room ${code} (map ${map})`);
				send(ws, { t: 'created', code, token: slot.token });
				break;
			}

			case 'join': {
				const code = String(msg.code || '').toUpperCase();
				const target = rooms.get(code);
				if (!target) return send(ws, { t: 'error', msg: 'No room with that code' });
				if (target.slots.length >= 2) return send(ws, { t: 'error', msg: 'That room is full' });
				if (target.slots[0].ws === ws) return send(ws, { t: 'error', msg: 'You are already in that room' });

				leaveCurrentRoom(ws);
				const slot = newSlot(ws);
				target.slots.push(slot);
				target.seed = Math.floor(Math.random() * 0x7fffffff);
				ws.room = code;
				log(`client ${ws.id} joined room ${code}`);
				send(ws, { t: 'joined', code, token: slot.token, map: target.map });
				sendToSlot(target.slots[0], { t: 'opponent_joined' });
				for (const s of target.slots) sendToSlot(s, { t: 'start', seed: target.seed, map: target.map });
				break;
			}

			case 'rejoin': {
				const code = String(msg.code || '').toUpperCase();
				const target = rooms.get(code);
				const slot = target && target.slots.find(s => s.token === msg.token);
				if (!slot) return send(ws, { t: 'error', msg: 'That match has ended' });

				// The old socket may not have noticed it's dead yet
				if (slot.ws && slot.ws !== ws) {
					slot.ws.room = null;
					slot.ws.slot = null;
					slot.ws.terminate();
				}
				leaveCurrentRoom(ws);
				clearTimeout(slot.graceTimer);
				slot.graceTimer = null;
				slot.ws = ws;
				ws.slot = slot;
				ws.room = code;

				const other = otherSlot(target, slot);
				// the game reports why it reconnected and its longest stall, to tell network drops apart
				// from a browser pausing the game
				const why = typeof msg.why === 'string' ? msg.why.replace(/[^a-z]/g, '').slice(0, 16) : '';
				log(`client ${ws.id} rejoined room ${code}` + (why ? ` (${why}, longest stall ${Number(msg.gap) | 0} ms${msg.hidden ? ', page hidden' : ''})` : ''));
				send(ws, {
					t: 'rejoined',
					code,
					go: target.started,
					opponentConnected: !!(other && other.ws),
					opponentReady: !!(other && target.ready.has(other)),
				});
				for (const frame of slot.queue) ws.send(frame, { binary: true });
				slot.queue = [];
				sendToSlot(other, { t: 'opponent_reconnected' });
				break;
			}

			case 'ready': {
				if (!room || room.slots.length < 2) return;
				room.ready.add(ws.slot);
				sendToSlot(otherSlot(room, ws.slot), { t: 'opponent_ready' });
				if (room.ready.size === 2 && !room.started) {
					room.started = true;
					for (const s of room.slots) sendToSlot(s, { t: 'go' });
				}
				break;
			}

			case 'leave':
				leaveCurrentRoom(ws);
				break;

			// Lets the game notice a dead connection quickly, whatever phase the match is in
			case 'ping':
				ws.sendsKeepalives = true;
				send(ws, { t: 'pong' });
				break;

			default:
				send(ws, { t: 'error', msg: `Unknown message "${msg.t}"` });
		}
	}

	function relayBinary(ws, data) {
		const room = ws.room && rooms.get(ws.room);
		const other = room && otherSlot(room, ws.slot);
		if (!other) return;

		if (other.ws && other.ws.readyState === other.ws.OPEN) {
			other.ws.send(data, { binary: true });
		} else if (other.queue.length < MAX_QUEUED_FRAMES) {
			// Held for when the opponent reconnects, so sent zombies aren't lost
			other.queue.push(data);
		}
	}

	const server = http.createServer((req, res) => serveStatic(staticDir, req, res));
	const wss = new WebSocketServer({ server });

	wss.on('connection', (ws, req) => {
		ws.id = nextClientId++;
		ws.room = null;
		ws.slot = null;
		ws.missedPongs = 0;
		ws.on('pong', () => { ws.missedPongs = 0; });
		log(`client ${ws.id} connected from ${req.headers['x-real-ip'] || req.socket.remoteAddress}`);

		ws.on('message', (data, isBinary) => {
			ws.lastSeen = Date.now();
			if (isBinary) return relayBinary(ws, data);

			let msg;
			try {
				msg = JSON.parse(data.toString());
			} catch {
				return send(ws, { t: 'error', msg: 'Bad JSON' });
			}
			handleControl(ws, msg);
		});

		ws.on('close', (code) => {
			log(`client ${ws.id} disconnected (code ${code})${ws.room ? ` from room ${ws.room}` : ''}`);
			socketClosed(ws);
		});
	});

	const heartbeat = setInterval(() => {
		for (const ws of wss.clients) {
			if (ws.missedPongs >= MAX_MISSED_PONGS) {
				log(`client ${ws.id} stopped answering pings`);
				ws.terminate();
				continue;
			}
			ws.missedPongs++;
			ws.ping();
		}
	}, HEARTBEAT_MS);

	const silenceCheck = setInterval(() => {
		const now = Date.now();
		for (const ws of wss.clients) {
			const room = ws.room && rooms.get(ws.room);
			if (ws.sendsKeepalives && room && room.slots.length === 2 && now - ws.lastSeen > MATCH_SILENCE_MS) {
				log(`client ${ws.id} went quiet for ${now - ws.lastSeen} ms`);
				ws.terminate();
			}
		}
	}, 1000);

	wss.on('close', () => {
		clearInterval(heartbeat);
		clearInterval(silenceCheck);
	});

	return new Promise(resolve => {
		server.listen(port, host, () => resolve({
			port: server.address().port,
			close: () => new Promise(done => {
				for (const room of rooms.values()) for (const slot of room.slots) clearTimeout(slot.graceTimer);
				for (const ws of wss.clients) ws.terminate();
				wss.close();
				server.close(done);
			}),
		}));
	});
}

module.exports = { createRelay };

if (require.main === module) {
	const args = parseArgs(process.argv.slice(2));
	createRelay(args).then(({ port }) => {
		console.log(`Versus relay listening on ws://localhost:${port}`);
		if (args.staticDir) console.log(`Serving ${args.staticDir} at http://localhost:${port}/`);
	});
}
