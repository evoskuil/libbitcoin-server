import { bytesToHex, hexToBytes } from './sha256';
import { sha256d, type Job } from './stratum';

// A block template (getblocktemplate), as the miner uses it.
export interface Template {
  height: number;
  version: number;
  previous: string;
  bits: string;
  time: number;
  value: bigint;
  txids: string[];
  data: string[];
  commitment?: string;
  longpollid?: string;
}

// Bytes of the extranonce rolled into the coinbase script.
export const EXTRANONCE_SIZE = 8;

// Bytes of the coinbase tag, which shares the script with height and
// extranonce (at most 100 bytes).
const TAG_SIZE = 64;

const hex = (value: unknown): value is string => typeof value === 'string' && /^([0-9a-f]{2})*$/i.test(value);

export function parseTemplate(result: unknown): Template | undefined {
  if (!result || typeof result !== 'object') return undefined;
  const value = result as Record<string, unknown>;
  const { height, version, previousblockhash, bits, curtime, coinbasevalue, transactions } = value;
  if (typeof height !== 'number' || typeof version !== 'number' || typeof curtime !== 'number') return undefined;
  if (!hex(previousblockhash) || previousblockhash.length !== 64 || !hex(bits) || bits.length !== 8) return undefined;
  if (typeof coinbasevalue !== 'number' || !Array.isArray(transactions)) return undefined;
  const txids: string[] = [];
  const data: string[] = [];
  for (const entry of transactions as unknown[]) {
    if (!entry || typeof entry !== 'object') return undefined;
    const tx = entry as Record<string, unknown>;
    if (!hex(tx.txid) || tx.txid.length !== 64 || !hex(tx.data)) return undefined;
    txids.push(tx.txid);
    data.push(tx.data);
  }

  const commitment = value.default_witness_commitment;
  return {
    height,
    version,
    previous: previousblockhash,
    bits,
    time: curtime,
    value: BigInt(coinbasevalue),
    txids,
    data,
    commitment: hex(commitment) && commitment ? commitment : undefined,
    longpollid: typeof value.longpollid === 'string' ? value.longpollid : undefined,
  };
}

function concat(...parts: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((sum, part) => sum + part.length, 0));
  let offset = 0;
  for (const part of parts) {
    out.set(part, offset);
    offset += part.length;
  }
  return out;
}

function littleEndian(value: bigint, size: number): Uint8Array {
  const out = new Uint8Array(size);
  for (let i = 0; i < size; i++) out[i] = Number((value >> BigInt(8 * i)) & 0xffn);
  return out;
}

export function varint(value: number): Uint8Array {
  if (value < 0xfd) return Uint8Array.of(value);
  if (value <= 0xffff) return concat(Uint8Array.of(0xfd), littleEndian(BigInt(value), 2));
  return concat(Uint8Array.of(0xfe), littleEndian(BigInt(value), 4));
}

// The minimal push of the height (bip34), a small number as its opcode.
function heightPush(height: number): Uint8Array {
  if (height === 0) return Uint8Array.of(0x00);
  if (height <= 16) return Uint8Array.of(0x50 + height);
  const bytes: number[] = [];
  for (let value = height; value > 0; value = Math.floor(value / 256)) bytes.push(value % 256);
  if ((bytes[bytes.length - 1] & 0x80) !== 0) bytes.push(0);
  return Uint8Array.of(bytes.length, ...bytes);
}

function tagPush(tag: string): Uint8Array {
  const bytes = new TextEncoder().encode(tag).slice(0, TAG_SIZE);
  return bytes.length === 0 ? new Uint8Array() : concat(Uint8Array.of(bytes.length), bytes);
}

function output(value: bigint, script: Uint8Array): Uint8Array {
  return concat(littleEndian(value, 8), varint(script.length), script);
}

// The merkle branch of the coinbase over the template transactions, from
// their txids (as displayed).
function merkleBranch(txids: string[]): string[] {
  const branch: string[] = [];
  let level: (Uint8Array | undefined)[] = [undefined, ...txids.map((txid) => hexToBytes(txid).reverse())];
  while (level.length > 1) {
    branch.push(bytesToHex(level[1] as Uint8Array));
    if (level.length % 2 !== 0) level.push(level[level.length - 1]);
    const next: (Uint8Array | undefined)[] = [undefined];
    for (let i = 2; i < level.length; i += 2) {
      next.push(sha256d(concat(level[i] as Uint8Array, level[i + 1] as Uint8Array)));
    }
    level = next;
  }
  return branch;
}

// Reverses the bytes within each 4 byte word.
function swapWords(bytes: Uint8Array): Uint8Array {
  const out = new Uint8Array(bytes.length);
  for (let i = 0; i < bytes.length; i += 4) {
    out[i] = bytes[i + 3];
    out[i + 1] = bytes[i + 2];
    out[i + 2] = bytes[i + 1];
    out[i + 3] = bytes[i];
  }
  return out;
}

// The template as a stratum job, its coinbase paying the script and carrying
// the tag, split around an extranonce of EXTRANONCE_SIZE bytes. The coinbase
// sequence is not final and its locktime is the prior height (bip54).
export function templateJob(template: Template, id: string, payout: string, tag: string): Job {
  const head = concat(heightPush(template.height), tagPush(tag));
  const script = head.length + 1 + EXTRANONCE_SIZE;
  const prefix = concat(
    littleEndian(1n, 4),
    Uint8Array.of(1),
    new Uint8Array(32),
    Uint8Array.of(0xff, 0xff, 0xff, 0xff),
    varint(script),
    head,
    Uint8Array.of(EXTRANONCE_SIZE)
  );

  const outputs = [output(template.value, hexToBytes(payout))];
  if (template.commitment) outputs.push(output(0n, hexToBytes(template.commitment)));
  const suffix = concat(
    Uint8Array.of(0xfe, 0xff, 0xff, 0xff),
    varint(outputs.length),
    ...outputs,
    littleEndian(BigInt(Math.max(0, template.height - 1)), 4)
  );

  return {
    id,
    previous: bytesToHex(swapWords(hexToBytes(template.previous).reverse())),
    prefix: bytesToHex(prefix),
    suffix: bytesToHex(suffix),
    branch: merkleBranch(template.txids),
    version: (template.version >>> 0).toString(16).padStart(8, '0'),
    bits: template.bits,
    time: (template.time >>> 0).toString(16).padStart(8, '0'),
    clean: true,
  };
}

// The block of a mined header (with its nonce), the coinbase of the job and
// extranonce, and the template transactions, as submitblock hex. A template
// with a witness commitment has a coinbase witness of the zero reserved value.
export function blockHex(header: Uint8Array, job: Job, extranonce: string, template: Template): string {
  const coinbase = hexToBytes(job.prefix + extranonce + job.suffix);
  const serialized = template.commitment
    ? concat(
        coinbase.subarray(0, 4),
        Uint8Array.of(0x00, 0x01),
        coinbase.subarray(4, coinbase.length - 4),
        Uint8Array.of(0x01, 0x20),
        new Uint8Array(32),
        coinbase.subarray(coinbase.length - 4)
      )
    : coinbase;

  return (
    bytesToHex(header) +
    bytesToHex(varint(template.data.length + 1)) +
    bytesToHex(serialized) +
    template.data.join('')
  );
}
