'use strict';

const { spawn } = require('node:child_process');
const { chromium } = require('playwright');
const fs = require('node:fs');
const path = require('node:path');

async function main() {
   const [probePath, chromePath, role, capturePath] = process.argv.slice(2);
   if (!probePath || !chromePath) throw new Error('Expected native probe and Chrome paths');
   const browser = await chromium.launch({
      executablePath: chromePath,
      headless: true,
      args: ['--no-sandbox', '--allow-loopback-in-peer-connection',
         '--disable-features=WebRtcHideLocalIpsWithMdns'],
   });
   const page = await browser.newPage();
   const probe = spawn(probePath, [], { stdio: ['pipe', 'pipe', 'pipe'],
      env: { ...process.env, ...(role === 'client' ? { KOTUKU_SCTP_DTLS_CLIENT: '1' } : {}) } });
   let output = '';
   let errors = '';
   let waiter = null;
   probe.stdout.setEncoding('utf8');
   probe.stderr.setEncoding('utf8');
   probe.stdout.on('data', (chunk) => {
      output += chunk;
      if (waiter) { waiter(); waiter = null; }
   });
   probe.stderr.on('data', (chunk) => { errors += chunk; });
   const waitFor = async (predicate) => {
      const deadline = Date.now() + 15000;
      while (!predicate()) {
         if (Date.now() >= deadline) throw new Error(`Native probe timed out: ${errors}`);
         await Promise.race([
            new Promise((resolve) => { waiter = resolve; }),
            new Promise((resolve) => setTimeout(resolve, 100)),
         ]);
      }
   };

   try {
      const offer = await page.evaluate(async () => {
         window.pc = new RTCPeerConnection();
         window.channels = [];
         window.failures = [];
         window.completed = 0;
         window.channelClosedCount = 0;
         const setup = (channel) => {
            window.channels.push(channel);
            if (channel.label.startsWith('native-')) {
               const mode = Number(channel.label.slice(7));
               if (channel.ordered !== (mode < 3) || channel.protocol !== 'echo' ||
                   channel.maxRetransmits !== (mode % 3 === 1 ? 0 : null) ||
                   channel.maxPacketLifeTime !== (mode % 3 === 2 ? 1000 : null)) {
                  window.failures.push(`${channel.label}: DCEP options differ`);
               }
            }
            channel.binaryType = 'arraybuffer';
            const expected = ['hello \u20ac \u0000', new Uint8Array([0, 255, 1, 0]).buffer, '', new ArrayBuffer(0)];
            let index = 0;
            channel.onmessage = ({ data }) => {
               const wanted = expected[index++];
               if (typeof wanted === 'string') {
                  if (data !== wanted) window.failures.push(`${channel.label}: text differs`);
               } else if (!(data instanceof ArrayBuffer) ||
                   String([...new Uint8Array(data)]) !== String([...new Uint8Array(wanted)])) {
                  window.failures.push(`${channel.label}: binary differs`);
               }
               if (index < expected.length) channel.send(expected[index]);
               else { window.completed++; if (!channel.label.startsWith('native-')) channel.close(); }
            };
            channel.onopen = () => channel.send(expected[0]);
            channel.onclose = () => { window.channelClosedCount++; };
            channel.onerror = () => window.failures.push(`${channel.label}: channel error`);
         };
         window.pc.ondatachannel = ({ channel }) => setup(channel);
         for (let mode = 0; mode < 6; ++mode) {
            const options = { ordered: mode < 3, protocol: 'echo' };
            if (mode % 3 === 1) options.maxRetransmits = 0;
            if (mode % 3 === 2) options.maxPacketLifeTime = 1000;
            setup(window.pc.createDataChannel(`browser-${mode}`, options));
         }
         setup(window.pc.createDataChannel('negotiated', { negotiated: true, id: 100 }));
         const description = await window.pc.createOffer();
         await window.pc.setLocalDescription(description);
         return description.sdp;
      });
      probe.stdin.end(offer);
      await waitFor(() => output.includes('\n'));
      const headerEnd = output.indexOf('\n');
      const answerLength = Number(output.slice(0, headerEnd));
      if (!Number.isInteger(answerLength) || answerLength < 100 || answerLength > 65536) {
         throw new Error(`Invalid native SDP length: ${output.slice(0, 100)} ${errors}`);
      }
      await waitFor(() => output.length >= headerEnd + 1 + answerLength);
      const answer = output.slice(headerEnd + 1, headerEnd + 1 + answerLength);
      await page.evaluate(async (sdp) => {
         await window.pc.setRemoteDescription({ type: 'answer', sdp });
      }, answer);
      await waitFor(() => output.includes('CONNECTED\n', headerEnd + 1 + answerLength));
      const state = await page.evaluate(() => window.pc.iceConnectionState);
      if (state !== 'connected' && state !== 'completed') {
         throw new Error(`Browser ICE state is ${state}; native output: ${output}; errors: ${errors}`);
      }
      const dtlsDeadline = Date.now() + 5000;
      let dtlsConnected = false;
      while (Date.now() < dtlsDeadline && !dtlsConnected) {
         dtlsConnected = await page.evaluate(async () => {
            const stats = await window.pc.getStats();
            return [...stats.values()].some((entry) => entry.type === 'transport' && entry.dtlsState === 'connected');
         });
         if (!dtlsConnected) await new Promise((resolve) => setTimeout(resolve, 100));
      }
      if (!dtlsConnected) throw new Error(`Browser DTLS did not connect; native errors: ${errors}`);
      await waitFor(() => output.includes('SCTP_CONNECTED\n', headerEnd + 1 + answerLength));
      const sctpDeadline = Date.now() + 5000;
      let sctpConnected = false;
      while (Date.now() < sctpDeadline && !sctpConnected) {
         sctpConnected = await page.evaluate(() => window.pc.sctp?.state === 'connected');
         if (!sctpConnected) await new Promise((resolve) => setTimeout(resolve, 20));
      }
      if (!sctpConnected) throw new Error(`Browser SCTP did not connect: ${errors}`);
      const messagesDeadline = Date.now() + 8000;
      let result;
      while (Date.now() < messagesDeadline) {
         result = await page.evaluate(() => ({ completed: window.completed, closed: window.channelClosedCount,
            failures: window.failures, channels: window.channels.map(channel => ({ label: channel.label,
               id: channel.id, state: channel.readyState, ordered: channel.ordered,
               maxRetransmits: channel.maxRetransmits, maxPacketLifeTime: channel.maxPacketLifeTime })) }));
         if (result.failures.length) throw new Error(JSON.stringify(result));
         if (result.completed === 13 && result.closed === 13) break;
         await new Promise(resolve => setTimeout(resolve, 20));
      }
      if (result.completed !== 13 || result.closed !== 13) {
         throw new Error(`Channels did not exchange and close: ${JSON.stringify(result)} ${errors}`);
      }
      await waitFor(() => output.includes('CHANNEL_CLOSED 13\n'));
      if (capturePath) {
         fs.mkdirSync(path.dirname(capturePath), { recursive: true });
         fs.writeFileSync(capturePath, JSON.stringify({
            browser: browser.version(), playwright: require('playwright/package.json').version,
            nativeDtlsRole: role, offer, answer, channels: result,
            packets: errors.split('\n').filter(line => /^SCTP_(RX|TX) /.test(line)),
         }, null, 2) + '\n');
      }
      console.log(`Browser and native SCTP channels exchanged text/binary/empty messages and closed (native DTLS ${role})`);
   } finally {
      probe.kill();
      await page.close();
      await browser.close();
   }
}

main().catch((error) => { console.error(error); process.exitCode = 1; });
