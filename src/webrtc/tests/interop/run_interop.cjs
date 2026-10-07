#!/usr/bin/env node
// WebRTC browser interop harness (Phase 0).
//
// Starts the Kōtuku signalling server under origo, drives headless Chromium through Playwright, and checks two
// scenarios:
//
//   pair   Two browser peers negotiate a data channel through the Kōtuku signalling relay and exchange a message.
//          Proves the harness, the relay and trickle ICE.  The offer and answer SDP are saved as parser fixtures.
//   probe  A browser peer negotiates against the Kōtuku ICE-lite probe.  The probe authenticates the browser's STUN
//          connectivity checks, answers them, and must then receive the browser's DTLS ClientHello on its UDP socket.
//
// Usage (CommonJS so that NODE_PATH can locate a globally installed playwright package):
//
//   NODE_PATH=$(npm root -g) node run_interop.cjs --origo build/agents-install/origo \
//      --script src/webrtc/tests/interop/signalling_server.tiri [--port 9310] [--out dir] [--headed]

'use strict';

const { spawn } = require('child_process');
const fs = require('fs');
const net = require('net');
const path = require('path');

let chromium;
try {
   ({ chromium } = require('playwright'));
}
catch (e) {
   console.error('The playwright package is not installed.  Install it with: npm install -g playwright');
   process.exit(2);
}

// Chromium hides host addresses behind mDNS names and ignores loopback interfaces by default.  Both would prevent
// a single-host test from forming candidate pairs, so they are disabled.

const CHROMIUM_ARGS = [
   '--allow-loopback-in-peer-connection',
   '--disable-features=WebRtcHideLocalIpsWithMdns',
];

const SCENARIO_TIMEOUT_MS = 20000;

function parse_args(argv) {
   const options = { port: 9310, out: null, headed: false, origo: null, script: null };
   for (let i = 2; i < argv.length; i++) {
      const arg = argv[i];
      if (arg === '--origo') options.origo = argv[++i];
      else if (arg === '--script') options.script = argv[++i];
      else if (arg === '--port') options.port = Number(argv[++i]);
      else if (arg === '--out') options.out = argv[++i];
      else if (arg === '--headed') options.headed = true;
      else throw new Error(`Unknown argument ${arg}`);
   }
   if (!options.origo || !options.script) throw new Error('--origo and --script are required');
   return options;
}

function wait_for_port(port, timeout_ms) {
   const deadline = Date.now() + timeout_ms;
   return new Promise((resolve, reject) => {
      const attempt = () => {
         const socket = net.connect(port, '127.0.0.1');
         socket.once('connect', () => { socket.destroy(); resolve(); });
         socket.once('error', () => {
            socket.destroy();
            if (Date.now() > deadline) reject(new Error(`Port ${port} did not open`));
            else setTimeout(attempt, 100);
         });
      };
      attempt();
   });
}

//---------------------------------------------------------------------------------------------------------------------
// Browser-side code.  These functions are serialised by page.evaluate() and run inside Chromium.

async function browser_pair_peer({ url, room, offerer, timeout }) {
   const ws = new WebSocket(url);
   await new Promise((resolve, reject) => {
      ws.onopen = resolve;
      ws.onerror = () => reject(new Error('WebSocket connection failed'));
   });

   const result = { offer: null, answer: null, local_candidates: [], messages: [], ice_states: [], pair: null };
   const pc = new RTCPeerConnection();
   const send = (msg) => ws.send(JSON.stringify(msg));

   pc.oniceconnectionstatechange = () => result.ice_states.push(pc.iceConnectionState);
   pc.onicecandidate = (event) => {
      if (event.candidate) {
         result.local_candidates.push(event.candidate.candidate);
         send({ type: 'candidate', candidate: event.candidate.toJSON() });
      }
   };

   const done = new Promise((resolve, reject) => {
      setTimeout(() => reject(new Error(`Timed out, ICE state ${pc.iceConnectionState}`)), timeout);

      const attach = (channel) => {
         channel.onopen = () => { if (offerer) channel.send('ping'); };
         channel.onmessage = (event) => {
            result.messages.push(event.data);
            if (offerer) resolve();
            else { channel.send(`pong:${event.data}`); resolve(); }
         };
      };

      if (offerer) attach(pc.createDataChannel('interop'));
      else pc.ondatachannel = (event) => attach(event.channel);
   });

   ws.onmessage = async (event) => {
      const msg = JSON.parse(event.data);
      if (msg.type === 'peer-joined' && offerer) {
         const offer = await pc.createOffer();
         await pc.setLocalDescription(offer);
         result.offer = offer.sdp;
         send({ type: 'offer', sdp: offer.sdp });
      }
      else if (msg.type === 'offer') {
         await pc.setRemoteDescription({ type: 'offer', sdp: msg.sdp });
         const answer = await pc.createAnswer();
         await pc.setLocalDescription(answer);
         result.offer = msg.sdp;
         result.answer = answer.sdp;
         send({ type: 'answer', sdp: answer.sdp });
      }
      else if (msg.type === 'answer') {
         result.answer = msg.sdp;
         await pc.setRemoteDescription({ type: 'answer', sdp: msg.sdp });
      }
      else if (msg.type === 'candidate') {
         await pc.addIceCandidate(msg.candidate);
      }
   };

   send({ type: 'join', room });
   await done;

   // Record the selected candidate pair for the report.
   const stats = await pc.getStats();
   stats.forEach((report) => {
      if (report.type === 'candidate-pair' && report.nominated && report.state === 'succeeded') {
         const local = stats.get(report.localCandidateId);
         const remote = stats.get(report.remoteCandidateId);
         result.pair = {
            local: `${local.candidateType} ${local.address}:${local.port}`,
            remote: `${remote.candidateType} ${remote.address}:${remote.port}`,
            rtt: report.currentRoundTripTime,
         };
      }
   });

   pc.close();
   ws.close();
   return result;
}

async function browser_probe_peer({ url, timeout, settle }) {
   const ws = new WebSocket(url);
   await new Promise((resolve, reject) => {
      ws.onopen = resolve;
      ws.onerror = () => reject(new Error('WebSocket connection failed'));
   });

   const result = { offer: null, answer: null, reports: [], ice_states: [], connection_states: [] };
   const pc = new RTCPeerConnection();
   pc.createDataChannel('probe');
   pc.oniceconnectionstatechange = () => result.ice_states.push(pc.iceConnectionState);
   pc.onconnectionstatechange = () => result.connection_states.push(pc.connectionState);

   const done = new Promise((resolve) => {
      const timer = setTimeout(resolve, timeout); // The caller judges the outcome from the reports
      ws.onmessage = async (event) => {
         const msg = JSON.parse(event.data);
         if (msg.type === 'probe-answer') {
            result.answer = msg.sdp;
            await pc.setRemoteDescription({ type: 'answer', sdp: msg.sdp });
         }
         else if (msg.type === 'probe-report') {
            result.reports.push(msg);
            // Keep listening after the first ClientHello so that nominating checks and DTLS retransmissions, which
            // can follow it, are captured.
            if ((msg.event === 'dtls') && (msg.count === 1)) { clearTimeout(timer); setTimeout(resolve, settle); }
         }
         else if (msg.type === 'error') {
            result.reports.push(msg);
         }
      };
   });

   const offer = await pc.createOffer();
   await pc.setLocalDescription(offer);
   result.offer = offer.sdp;
   ws.send(JSON.stringify({ type: 'probe', sdp: offer.sdp }));

   await done;
   pc.close();
   ws.close();
   return result;
}

//---------------------------------------------------------------------------------------------------------------------

async function run_pair(browser, url) {
   const context = await browser.newContext();
   const [offerer_page, answerer_page] = [await context.newPage(), await context.newPage()];
   const room = `pair-${Date.now()}`;

   // The offerer joins first and waits for the answerer's peer-joined notification before creating its offer.
   const offerer = offerer_page.evaluate(browser_pair_peer, { url, room, offerer: true, timeout: SCENARIO_TIMEOUT_MS });
   await new Promise((resolve) => setTimeout(resolve, 250));
   const answerer = answerer_page.evaluate(browser_pair_peer,
      { url, room, offerer: false, timeout: SCENARIO_TIMEOUT_MS });

   const [a, b] = await Promise.all([offerer, answerer]);
   await context.close();

   const checks = [
      [a.messages.includes('pong:ping'), 'Offerer received the echoed message'],
      [b.messages.includes('ping'), 'Answerer received the message'],
      [a.ice_states.includes('connected') || a.ice_states.includes('completed'), 'Offerer ICE connected'],
   ];
   return { name: 'pair', checks, data: { offerer: a, answerer: b } };
}

async function run_probe(browser, url) {
   const context = await browser.newContext();
   const page = await context.newPage();
   const r = await page.evaluate(browser_probe_peer, { url, timeout: SCENARIO_TIMEOUT_MS, settle: 3000 });
   await context.close();

   const stun = r.reports.filter((m) => m.event === 'stun');
   const valid = stun.filter((m) => m.valid);
   const dtls = r.reports.filter((m) => m.event === 'dtls');
   const invalid = stun.filter((m) => !m.valid).map((m) => m.reason);

   const reasons = invalid.length ? `: ${[...new Set(invalid)].join('; ')}` : '';
   const checks = [
      [valid.length > 0,
         `Browser STUN checks authenticated by Kōtuku (${valid.length} valid, ${invalid.length} invalid${reasons})`],
      [valid.some((m) => m.use_candidate), 'Browser nominated the Kōtuku candidate (USE-CANDIDATE)'],
      [r.ice_states.includes('connected') || r.ice_states.includes('completed'),
         `Browser ICE connected against the probe (states: ${r.ice_states.join(', ') || 'none'})`],
      [dtls.length > 0 && dtls[0].first_byte === 22 && dtls[0].handshake_type === 1,
         'DTLS ClientHello received on the Kōtuku socket'],
   ];
   return { name: 'probe', checks, data: r };
}

async function main() {
   const options = parse_args(process.argv);
   const url = `ws://127.0.0.1:${options.port}/`;

   const origo = spawn(options.origo,
      [options.script, `port=${options.port}`, `probe-port=${options.port + 1}`, '--log-warning'],
      { stdio: ['ignore', 'inherit', 'inherit'] });
   let origo_exited = false;
   origo.on('exit', () => { origo_exited = true; });

   let browser;
   let failed = 0;
   try {
      await wait_for_port(options.port, 15000);
      browser = await chromium.launch({ headless: !options.headed, args: CHROMIUM_ARGS });
      console.log(`Chromium ${browser.version()}`);

      const results = [];
      for (const scenario of [run_pair, run_probe]) {
         try {
            results.push(await scenario(browser, url));
         }
         catch (e) {
            results.push({ name: scenario.name, checks: [[false, `Scenario threw: ${e.message}`]], data: null });
         }
      }

      for (const result of results) {
         console.log(`Scenario ${result.name}:`);
         for (const [ok, label] of result.checks) {
            console.log(`  ${ok ? 'PASS' : 'FAIL'} ${label}`);
            if (!ok) failed++;
         }
      }

      const pair = results.find((r) => r.name === 'pair');
      if (pair && pair.data && pair.data.offerer.pair) {
         console.log(`  pair selected: ${pair.data.offerer.pair.local} -> ${pair.data.offerer.pair.remote}`);
      }

      if (options.out) {
         fs.mkdirSync(options.out, { recursive: true });
         fs.writeFileSync(path.join(options.out, 'report.json'), JSON.stringify(results, null, 2));
         if (pair && pair.data) {
            fs.writeFileSync(path.join(options.out, 'chromium_datachannel_offer.sdp'), pair.data.offerer.offer || '');
            fs.writeFileSync(path.join(options.out, 'chromium_datachannel_answer.sdp'),
               pair.data.answerer.answer || '');
         }
         console.log(`Results written to ${options.out}`);
      }
   }
   catch (e) {
      console.error(`Harness error: ${e.message}`);
      failed++;
   }
   finally {
      if (browser) await browser.close();
      if (!origo_exited) origo.kill('SIGTERM');
   }

   console.log(failed ? `${failed} check(s) failed` : 'All checks passed');
   process.exit(failed ? 1 : 0);
}

main();
