import { expect, test } from 'vitest';
import { bytesToHex, hexToBytes } from '@/lib/mining/sha256';
import { coinbaseHeight, coinbaseTag, sha256d } from '@/lib/mining/stratum';
import { blockHex, parseTemplate, templateJob } from '@/lib/mining/template';

// Mainnet block 100000, its coinbase txid and the txids that follow it.
const COINBASE = '8c14f0db3df150123e6f3dbbf30f8b955a8249b62ac1d1ff16284aefa3d06d87';
const TXIDS = [
  'fff2525b8931402dd09222c50775608f75787bd2b87e56995a7bdd30f79702c4',
  '6359f0868171b1d194cbee1af2f16ea598ae8fad666d9b012c8ed2b79a236ec4',
  'e9a66845e05d5abc0ad04ec80f774a7e585c6e8db975962d069a522137b80c1d',
];
const ROOT = 'f3e94742aca4b5ef85488dc37c06c3282295ffec960994b2c0d5ac2a25a95766';
const PREVIOUS = '000000000002d01c1fccc21636b607dfd930d31d01c3a62104612a1719011250';
const PAYOUT = '0014751e76e8199196d454941c45d1b3a323f1433bd6';
const COMMITMENT = '6a24aa21a9ed' + '11'.repeat(32);
const ZERO = '00'.repeat(32);

function template(height: number, commitment?: string) {
  return parseTemplate({
    height,
    version: 0x20000000,
    previousblockhash: PREVIOUS,
    bits: '1b04864c',
    curtime: 1293623863,
    coinbasevalue: 5000000000,
    transactions: TXIDS.map((txid) => ({ txid, data: 'abcd' })),
    default_witness_commitment: commitment,
    longpollid: `${PREVIOUS}1`,
  })!;
}

// The merkle root (as displayed) of a coinbase txid folded with a branch.
function fold(txid: string, branch: string[]): string {
  let root = hexToBytes(txid).reverse();
  for (const hash of branch) root = sha256d(new Uint8Array([...root, ...hexToBytes(hash)]));
  return bytesToHex(root.reverse());
}

test('parseTemplate reads a template', () => {
  const value = template(100000, COMMITMENT);
  expect(value.height).toBe(100000);
  expect(value.value).toBe(5000000000n);
  expect(value.txids).toEqual(TXIDS);
  expect(value.commitment).toBe(COMMITMENT);
  expect(value.longpollid).toBe(`${PREVIOUS}1`);
});

test('parseTemplate rejects a template without transactions', () => {
  expect(parseTemplate({ height: 1, version: 1, previousblockhash: PREVIOUS, bits: '1d00ffff', curtime: 1, coinbasevalue: 1 })).toBeUndefined();
});

test('templateJob branch folds the coinbase to the merkle root', () => {
  expect(fold(COINBASE, templateJob(template(100000), '1', PAYOUT, 'tag').branch)).toBe(ROOT);
});

test('templateJob coinbase commits the height and tag', () => {
  const job = templateJob(template(100000), '1', PAYOUT, 'libbitcoin');
  expect(coinbaseHeight(job)).toBe(100000);
  expect(coinbaseTag(job)).toBe('libbitcoin');
  expect(job.version).toBe('20000000');
  expect(job.bits).toBe('1b04864c');
});

test('templateJob pushes a small height as its opcode', () => {
  expect(templateJob(template(16), '1', PAYOUT, '').prefix.slice(82, 86)).toBe('0a60');
});

test('templateJob coinbase pays the payout and the commitment', () => {
  expect(templateJob(template(100000, COMMITMENT), '1', PAYOUT, '').suffix).toBe(`feffffff0200f2052a01000000160014751e76e8199196d454941c45d1b3a323f1433bd6000000000000000026${COMMITMENT}9f860100`);
});

test('blockHex serializes the coinbase with its witness reserved value', () => {
  const value = template(100000, COMMITMENT);
  const job = templateJob(value, '1', PAYOUT, '');
  const block = blockHex(new Uint8Array(80), job, '00'.repeat(8), value);
  expect(block.slice(160, 162)).toBe('04');
  expect(block.slice(162, 174)).toBe('010000000001');
  expect(block.endsWith(`0120${ZERO}9f860100abcdabcdabcd`)).toBe(true);
});

test('blockHex serializes a coinbase without witness absent a commitment', () => {
  const value = template(100000);
  const job = templateJob(value, '1', PAYOUT, '');
  const block = blockHex(new Uint8Array(80), job, '00'.repeat(8), value);
  expect(block.slice(160)).toBe(`04${job.prefix}${'00'.repeat(8)}${job.suffix}abcdabcdabcd`);
});
