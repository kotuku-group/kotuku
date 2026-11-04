'use strict';
const { spawn } = require('node:child_process');
const { chromium } = require('playwright');
const fs = require('node:fs');
const path = require('node:path');
const net = require('node:net');
async function exercise({ port, nativeOffer }) {
   const pc = new RTCPeerConnection({ iceServers: [] });
   const ws = new WebSocket(`ws://127.0.0.1:${port}/`);
   const replies = [], pending = [], nativeCandidates = [], nativeStates = [];
   let channel, failure, nativeStats, restartPhase, sequence = Promise.resolve();
   function attach(value) {
      channel = value; value.binaryType = 'arraybuffer';
      value.onmessage = event => replies.push(event.data);
   }
   pc.ondatachannel = event => attach(event.channel);
   if (!nativeOffer) attach(pc.createDataChannel('echo'));
   const partial = [];
   for (let mode = 0; mode < 6; ++mode) {
      const options = { ordered: mode < 3 };
      if (mode % 3 === 1) options.maxRetransmits = 2;
      // Keep queued-message preservation inside its lifetime even with Debug bulk head-of-line blocking.
      // Expiry during unavailable routes is asserted separately by the deterministic native migration fixture.
      if (mode % 3 === 2) options.maxPacketLifeTime = 30000;
      const value = pc.createDataChannel(`migration-${mode}`, options);
      const entry = { channel: value, replies: [] };
      value.binaryType = 'arraybuffer';
      value.onmessage = event => entry.replies.push(event.data);
      partial.push(entry);
   }
   const send = message => ws.send(JSON.stringify(message));
   pc.onicecandidate = ({ candidate }) => {
      if (candidate) send({ type: 'candidate', candidate: candidate.candidate });
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
            if (!message.candidate.includes('typ relay')) throw new Error('Native direct candidate leaked');
            nativeCandidates.push(message.candidate);
            const candidate = { candidate: message.candidate, sdpMid: '0', sdpMLineIndex: 0 };
            if (pc.remoteDescription) await pc.addIceCandidate(candidate); else pending.push(candidate);
         } else if (message.type === 'state') nativeStates.push(message);
         else if (message.type === 'stats') nativeStats = message;
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
      await wait(() => channel?.readyState === 'open' && partial.every(entry => entry.channel.readyState === 'open'));
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
      const ids = partial.map(entry => entry.channel.id);
      for (const nativeRestart of [false, true]) {
         restartPhase = nativeRestart ? 'native' : 'browser';
         const oldRemote = pc.remoteDescription.sdp;
         const count = replies.length;
         const counts = partial.map(entry => entry.replies.length);
         const payload = new Uint8Array(131072).fill(nativeRestart ? 73 : 91);
         const small = new Uint8Array(1024).fill(nativeRestart ? 19 : 23);
         for (const entry of partial) entry.channel.send(small);
         channel.send(payload);
         if (nativeRestart) send({ type: 'restart' });
         else {
            await pc.setLocalDescription(await pc.createOffer({ iceRestart: true }));
            send({ type: 'offer', sdp: pc.localDescription.sdp });
         }
         await wait(() => pc.remoteDescription.sdp !== oldRemote && replies.length === count + 1 &&
            partial.every((entry, index) => entry.replies.length === counts[index] + 1));
         function exact(actual, expected) {
            if (!(actual instanceof ArrayBuffer) || actual.byteLength !== expected.byteLength ||
                new Uint8Array(actual).some((byte, index) => byte !== expected[index])) {
               throw new Error('Queued migration payload differs');
            }
         }
         exact(replies[count], payload);
         partial.forEach((entry, index) => exact(entry.replies[counts[index]], small));
         await new Promise(resolve => setTimeout(resolve, 100));
         if (channel !== originalChannel || replies.length !== count + 1 ||
             !partial.every((entry, index) => entry.channel.id === ids[index] &&
                entry.channel.readyState === 'open' && entry.replies.length === counts[index] + 1)) {
            throw new Error('Restart replaced a channel or duplicated a message');
         }
      }
      const stats = [...(await pc.getStats()).values()];
      const transport = stats.find(value => value.type === 'transport' && value.selectedCandidatePairId);
      const pair = stats.find(value => value.id === transport?.selectedCandidatePairId);
      const remote = stats.find(value => value.id === pair?.remoteCandidateId);
      if (!remote || remote.candidateType !== 'relay' || !nativeCandidates.length) {
         throw new Error('Selected browser path did not prove native relay');
      }
      channel.close(); partial.forEach(entry => entry.channel.close());
      await wait(() => channel.readyState === 'closed' && partial.every(entry => entry.channel.readyState === 'closed'));
      return { nativeOffer, remoteCandidateType: remote.candidateType, messages: replies.length,
         migrationChannels: partial.length, restartInitiators: ['browser', 'native'], stats };
   } catch (error) {
      if (ws.readyState === WebSocket.OPEN) {
         send({ type: 'stats' });
         const deadline = Date.now() + 1000;
         while (!nativeStats && Date.now() < deadline) await new Promise(resolve => setTimeout(resolve, 10));
      }
      throw new Error(`${error.message}\nMigration diagnostics: ${JSON.stringify({ nativeOffer, restartPhase,
         native: nativeStats, mainReplies: replies.length,
         partial: partial.map(entry => ({ label: entry.channel.label, replies: entry.replies.length,
            state: entry.channel.readyState, lifetime: entry.channel.maxPacketLifeTime,
            retries: entry.channel.maxRetransmits })) })}`);
   } finally { pc.close(); ws.close(); }
}
async function main() {
   const [origo, script, chrome, config, output] = process.argv.slice(2);
   const reservation = net.createServer();
   await new Promise((resolve, reject) => {
      reservation.once('error', reject); reservation.listen(0, '127.0.0.1', resolve);
   });
   const port = reservation.address().port;
   await new Promise(resolve => reservation.close(resolve));
   const server = spawn(origo, [script, `port=${port}`, `config=${config}`, '--log-warning'],
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
         args: ['--no-sandbox', '--allow-loopback-in-peer-connection', '--disable-features=WebRtcHideLocalIpsWithMdns'] });
      const results = [];
      for (const nativeOffer of [false, true]) {
         const page = await browser.newPage();
         try { results.push(await page.evaluate(exercise, { port, nativeOffer })); }
         finally { await page.close(); }
      }
      fs.mkdirSync(path.dirname(output), { recursive: true });
      fs.writeFileSync(output, JSON.stringify({ browser: browser.version(), results }, null, 2));
      console.log('PASS Chromium/public native TURN both offers, exact payloads, reliable/partial restart both initiators, selected relay');
   } catch (error) { throw new Error(`${error.stack}\nNative log:\n${errors}`); }
   finally {
      if (browser) await browser.close();
      server.kill('SIGTERM');
      if (server.exitCode === null) await new Promise(resolve => server.once('exit', resolve));
   }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
