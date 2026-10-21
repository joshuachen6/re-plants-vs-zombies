#!/usr/bin/env node
// Relay server for the Versus mode of the web build.
//
// Two players meet in a room identified by a 4-letter code. The server understands only the lobby
// messages (JSON text frames); everything else (binary frames) is game data that it forwards
// untouched to the other player in the room.
//
//   client -> server   {"t":"create"}                  server -> client   {"t":"created","code":"ABCD"}
//                      {"t":"join","code":"ABCD"}                         {"t":"joined","code":"ABCD"}
//                      {"t":"ready"}                                      {"t":"opponent_joined"}
//                      {"t":"leave"}                                      {"t":"start","seed":123}
//                                                                         {"t":"opponent_ready"}
//                                                                         {"t":"go"}
//                                                                         {"t":"opponent_left"}
//                                                                         {"t":"error","msg":"..."}
//
// Usage: node relay.js [--port 8787] [--host 127.0.0.1] [--static ../build-web]

'use strict';

const http = require('http');
const fs = require('fs');
const path = require('path');
const { WebSocketServer } = require('ws');

// Clients are pinged this often and dropped after missing MAX_MISSED_PONGS replies in a row, so a
// short network stall doesn't end a match
const HEARTBEAT_MS = 15000;
const MAX_MISSED_PONGS = 2;
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

function createRelay({ port, host, staticDir, log = console.log }) {
	let nextClientId = 1;
	const rooms = new Map(); // code -> { players: [ws, ws?], ready: Set<ws> }

	function send(ws, msg) {
		if (ws && ws.readyState === ws.OPEN) ws.send(JSON.stringify(msg));
	}

	function opponentOf(ws) {
		const room = ws.room && rooms.get(ws.room);
		return room ? room.players.find(p => p !== ws) : undefined;
	}

	function newCode() {
		for (;;) {
			let code = '';
			for (let i = 0; i < 4; i++) code += CODE_LETTERS[Math.floor(Math.random() * CODE_LETTERS.length)];
			if (!rooms.has(code)) return code;
		}
	}

	function leaveRoom(ws) {
		const room = ws.room && rooms.get(ws.room);
		if (!room) return;
		send(opponentOf(ws), { t: 'opponent_left' });
		for (const p of room.players) p.room = null;
		rooms.delete(ws.room);
	}

	function handleControl(ws, msg) {
		switch (msg.t) {
			case 'create': {
				leaveRoom(ws);
				const code = newCode();
				rooms.set(code, { players: [ws], ready: new Set() });
				ws.room = code;
				log(`client ${ws.id} created room ${code}`);
				send(ws, { t: 'created', code });
				break;
			}

			case 'join': {
				const code = String(msg.code || '').toUpperCase();
				const room = rooms.get(code);
				if (!room) return send(ws, { t: 'error', msg: 'No room with that code' });
				if (room.players.length >= 2) return send(ws, { t: 'error', msg: 'That room is full' });
				if (room.players[0] === ws) return send(ws, { t: 'error', msg: 'You are already in that room' });

				leaveRoom(ws);
				room.players.push(ws);
				ws.room = code;
				log(`client ${ws.id} joined room ${code}`);
				send(ws, { t: 'joined', code });
				send(room.players[0], { t: 'opponent_joined' });

				const seed = Math.floor(Math.random() * 0x7fffffff);
				for (const p of room.players) send(p, { t: 'start', seed });
				break;
			}

			case 'ready': {
				const room = ws.room && rooms.get(ws.room);
				if (!room || room.players.length < 2) return;
				room.ready.add(ws);
				send(opponentOf(ws), { t: 'opponent_ready' });
				if (room.ready.size === 2) {
					for (const p of room.players) send(p, { t: 'go' });
				}
				break;
			}

			case 'leave':
				leaveRoom(ws);
				break;

			default:
				send(ws, { t: 'error', msg: `Unknown message "${msg.t}"` });
		}
	}

	const server = http.createServer((req, res) => serveStatic(staticDir, req, res));
	const wss = new WebSocketServer({ server });

	wss.on('connection', (ws, req) => {
		ws.id = nextClientId++;
		ws.room = null;
		ws.missedPongs = 0;
		ws.on('pong', () => { ws.missedPongs = 0; });
		log(`client ${ws.id} connected from ${req.headers['x-real-ip'] || req.socket.remoteAddress}`);

		ws.on('message', (data, isBinary) => {
			if (isBinary) {
				const opponent = opponentOf(ws);
				if (opponent && opponent.readyState === opponent.OPEN) opponent.send(data, { binary: true });
				return;
			}

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
			leaveRoom(ws);
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

	wss.on('close', () => clearInterval(heartbeat));

	return new Promise(resolve => {
		server.listen(port, host, () => resolve({
			port: server.address().port,
			close: () => new Promise(done => {
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
