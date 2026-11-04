'use strict';
const { spawn } = require('node:child_process');
const { chromium } = require('playwright');
const fs = require('node:fs');
const path = require('node:path');
const net = require('node:net');
async function exercise({ port, nativeOffer }) {
   const pc = new RTCPeerConnection({ iceServers: [] });
   const ws = new WebSocket(`ws://127.0.0.1:${port}/`);
   const replies = [], pending = [], nativeCandidates = [], browserCandidates = [], nativeStates = [];
   let channel, failure, sequence = Promise.resolve();
   function attach(value) {
      channel = value; value.binaryType = 'arraybuffer';
      value.onmessage = event => replies.push(event.data);
   }
   pc.ondatachannel = event => attach(event.channel);
   if (!nativeOffer) attach(pc.createDataChannel('echo'));
   const send = message => ws.send(JSON.stringify(message));
   pc.onicecandidate = ({ candidate }) => {
      if (candidate) {
         browserCandidates.push(candidate.candidate);
         if (!candidate.candidate.includes('.local') || !candidate.candidate.includes('typ host')) {
            failure = 'Browser did not obfuscate its host candidate'; return;
         }
         send({ type: 'candidate', candidate: candidate.candidate });
      }
   };
   const description = async message => {
      await pc.setRemoteDescription(message);
      for (const candidate of pending.splice(0)) await pc.addIceCandidate(candidate);
   };
   ws.onmessage = ({ data }) => {
      sequence = sequence.then(async () => {
         const message = JSON.parse(data);
         if (message.type === 'ready') {
            await pc.setLocalDescription(await pc.createOffer());
            send({ type: 'offer', sdp: pc.localDescription.sdp });
         } else if (message.type === 'offer') {
            await description({ type: 'offer', sdp: message.sdp });
            await pc.setLocalDescription(await pc.createAnswer());
            send({ type: 'answer', sdp: pc.localDescription.sdp });
         } else if (message.type === 'answer') await description({ type: 'answer', sdp: message.sdp });
         else if (message.type === 'candidate') {
            if (!message.candidate.includes('.local') || !message.candidate.includes('typ host')) {
               throw new Error('Native numeric host leaked');
            }
            nativeCandidates.push(message.candidate);
            const candidate = { candidate: message.candidate, sdpMid: '0', sdpMLineIndex: 0 };
            if (pc.remoteDescription) await pc.addIceCandidate(candidate); else pending.push(candidate);
         } else if (message.type === 'state') nativeStates.push(message);
         else if (message.type === 'error') throw new Error(`Native error ${message.message}`);
      }).catch(error => { failure = error.message; });
   };
   async function wait(condition) {
      const deadline = Date.now() + 20000;
      while (!condition()) {
         if (failure) throw new Error(failure);
         if (Date.now() > deadline) throw new Error(`Timeout; ICE=${pc.iceConnectionState}; native=${JSON.stringify(nativeStates)}; ` +
            `candidates=${JSON.stringify(nativeCandidates)}; stats=${JSON.stringify([...(await pc.getStats()).values()])}`);
         await new Promise(resolve => setTimeout(resolve, 5));
      }
   }
   try {
      await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
      send({ type: 'join', nativeOffer });
      await wait(() => channel?.readyState === 'open');
      for (const payload of ['hello € \0', '', new ArrayBuffer(0),
         new Uint8Array([0, 255, 128, 1]), new Uint8Array(65536).fill(91)]) {
         const count = replies.length;
         channel.send(payload); await wait(() => replies.length === count + 1);
         const actual = replies[count];
         if (typeof payload === 'string') {
            if (actual !== payload) throw new Error('Text differs');
         } else {
            const expected = new Uint8Array(payload.buffer || payload);
            if (!(actual instanceof ArrayBuffer) || actual.byteLength !== expected.byteLength ||
               new Uint8Array(actual).some((byte, index) => byte !== expected[index])) throw new Error('Binary differs');
         }
      }
      const originalChannel = channel;
      for (const nativeRestart of [false, true]) {
         const oldRemote = pc.remoteDescription.sdp;
         const count = replies.length;
         const payload = new Uint8Array(131072).fill(nativeRestart ? 73 : 91);
         channel.send(payload);
         if (nativeRestart) send({ type: 'restart' });
         else {
            await pc.setLocalDescription(await pc.createOffer({ iceRestart: true }));
            send({ type: 'offer', sdp: pc.localDescription.sdp });
         }
         await wait(() => pc.remoteDescription.sdp !== oldRemote && replies.length === count + 1);
         const actual = replies[count];
         if (!(actual instanceof ArrayBuffer) || actual.byteLength !== payload.byteLength ||
             new Uint8Array(actual).some((byte, index) => byte !== payload[index])) {
            throw new Error('Queued reliable data changed during restart');
         }
         const after = `after-restart-${nativeRestart}`;
         channel.send(after);
         await wait(() => replies.length === count + 2);
         await new Promise(resolve => setTimeout(resolve, 100));
         if (replies.length !== count + 2 || replies[count + 1] !== after || channel !== originalChannel) {
            throw new Error('Restart duplicated data or replaced channel');
         }
      }
      const stats = [...(await pc.getStats()).values()];
      const transport = stats.find(value => value.type === 'transport' && value.selectedCandidatePairId);
      const pair = stats.find(value => value.id === transport?.selectedCandidatePairId);
      const remote = stats.find(value => value.id === pair?.remoteCandidateId);
      if (!remote || remote.candidateType !== 'host' || !nativeCandidates.length || !browserCandidates.length) {
         throw new Error('Selected browser path did not prove obfuscated native host');
      }
      channel.close(); await wait(() => channel.readyState === 'closed');
      return { nativeOffer, remoteCandidateType: remote.candidateType, messages: replies.length, nativeCandidates, browserCandidates, stats };
   } finally { pc.close(); ws.close(); }
}
async function main() {
   const [origo, script, chrome, output] = process.argv.slice(2);
   const port = 9324;
   const server = spawn(origo, [script, `port=${port}`, '--log-warning'],
      { stdio: ['ignore', 'ignore', 'pipe'] });
   let errors = ''; server.stderr.on('data', bytes => { errors = (errors + bytes).slice(-8192); });
   let browser;
   try {
      let ready = false;
      for (let i = 0; i < 100 && !ready; ++i) {
         if (server.exitCode !== null) throw new Error('Signalling process exited');
         ready = await new Promise(resolve => {
            const socket = net.connect(port, '127.0.0.1');
            socket.on('connect', () => { socket.destroy(); resolve(true); });
            socket.on('error', () => resolve(false));
         });
         if (!ready) await new Promise(resolve => setTimeout(resolve, 50));
      }
      if (!ready) throw new Error('Signalling startup timeout');
      browser = await chromium.launch({ executablePath: chrome, headless: true,
         args: ['--no-sandbox'] });
      const results = [];
      for (const nativeOffer of [false, true]) {
         const page = await browser.newPage();
         try { results.push(await page.evaluate(exercise, { port, nativeOffer })); }
         finally { await page.close(); }
      }
      fs.mkdirSync(path.dirname(output), { recursive: true });
      fs.writeFileSync(output, JSON.stringify({ browser: browser.version(), results }, null, 2));
      console.log('PASS Chromium/public native mDNS both offers, obfuscated candidates in both directions, exact messages and reliable restart both initiators');
   } catch (error) { throw new Error(`${error.stack}\nNative log:\n${errors}`); }
   finally {
      if (browser) await browser.close();
      server.kill('SIGTERM');
      if (server.exitCode === null) await new Promise(resolve => server.once('exit', resolve));
   }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
