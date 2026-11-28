import MinerWorker from './miner.worker?worker&inline';
import type { WorkerRequest, WorkerResponse } from './miner.worker';
import { bytesToHex, headerWork } from './sha256';
import {
  bitsHex,
  openStandardMiningChannel,
  parseFrame,
  setupConnection,
  standardHeader,
  submitSharesStandard,
  type ServerMessage,
} from './sv2';
import { blockHex, EXTRANONCE_SIZE, parseTemplate, templateJob, type Template } from './template';
import {
  bitsTarget,
  buildHeader,
  coinbaseHeight,
  coinbaseTag,
  difficultyTarget,
  extranonce,
  hashValue,
  headerHash,
  parseJob,
  previousHash,
  submitParams,
  suggestedDifficulty,
  targetDifficulty,
  THREAD_RATE,
  targetWords,
  withNonce,
  type Job,
} from './stratum';

export type MinerProtocol = 'v1' | 'v2' | 'bitcoind' | 'btcd';

// A protocol of block templates (getblocktemplate), mined to the block target.
export function isTemplate(protocol?: MinerProtocol): boolean {
  return protocol === 'bitcoind' || protocol === 'btcd';
}

export interface MinerSettings {
  url: string;
  worker: string;
  password: string;
  threads: number;
  protocol?: MinerProtocol;
  // The coinbase output script (hex) and tag of a template protocol.
  payout?: string;
  tag?: string;
  // The username authenticated with the password (btcd).
  user?: string;
}

export interface HashrateSample {
  time: number;
  rate: number;
}

export type MinerStatus = 'stopped' | 'connecting' | 'waiting' | 'mining' | 'failed';

export type ShareOutcome = 'pending' | 'accepted' | 'rejected' | 'unanswered';

export interface Share {
  key: number;
  time: number;
  job: string;
  height?: number;
  nonce: string;
  hash: string;
  difficulty: number;
  block: boolean;
  outcome: ShareOutcome;
  reason?: string;
}

export interface MinerJob {
  id: string;
  height?: number;
  tag?: string;
  previous: string;
  networkDifficulty: number;
  time: number;
  clean: boolean;
  received: number;
}

export interface MinerState {
  status: MinerStatus;
  message?: string;
  settings?: MinerSettings;
  started?: number;
  difficulty?: number;
  job?: MinerJob;
  jobs: number;
  hashrate: number;
  // True if the workers hash with WebAssembly SIMD, false with JavaScript.
  simd?: boolean;
  history: HashrateSample[];
  hashes: number;
  accepted: number;
  rejected: number;
  blocks: number;
  shares: Share[];
}

// Shares kept for display.
const SHARES = 50;

// Seconds of hashrate kept for display, one sample per second.
const HISTORY = 300;

// Seconds of hashing averaged by the hashrate.
const WINDOW = 10;

// Nonce space of one unit of work (all of it, per extranonce2).
const NONCES = 2 ** 32;

// Seconds a share waits for its answer before the connection is taken as lost.
const ANSWER = 30;

// Seconds before reconnecting a lost connection, doubled per attempt to a cap.
const RETRY = 5;
const RETRY_CAP = 60;

// Seconds between template requests, for changes (time, work) not polled.
const REFRESH = 30;

const SUBSCRIBE = 1;
const AUTHORIZE = 2;
const SUGGEST = 3;
const TEMPLATE = 4;
const RENEW = 5;
const AUTHENTICATE = 6;
const NOTIFY = 7;

const initial: MinerState = {
  status: 'stopped',
  jobs: 0,
  hashrate: 0,
  history: [],
  hashes: 0,
  accepted: 0,
  rejected: 0,
  blocks: 0,
  shares: [],
};

// The totals, shares and history of a run, kept by a restart of the same
// stratum service and worker.
export function carried(state: MinerState, settings: MinerSettings): Partial<MinerState> {
  if (
    state.settings?.url !== settings.url ||
    state.settings.worker !== settings.worker ||
    (state.settings.protocol ?? 'v1') !== (settings.protocol ?? 'v1')
  )
    return {};
  const { jobs, history, hashes, accepted, rejected, blocks, shares } = state;
  return { jobs, history, hashes, accepted, rejected, blocks, shares };
}

// A stratum v2 standard job, made current by its previous block.
interface StandardJob {
  job: number;
  height?: number;
  version: number;
  merkle: Uint8Array;
  previous: Uint8Array;
  ntime: number;
  bits: number;
}

// A unit of work: the header hashed, and what its share submits.
interface Unit {
  header: Uint8Array;
  id: string;
  bits: string;
  height?: number;
  job?: Job;
  extranonce2?: string;
  standard?: StandardJob;
  template?: Template;
}

// The maximum target of a standard channel, any difficulty the server sets.
const MAXIMUM_TARGET = (1n << 256n) - 1n;

function reason(error: unknown): string {
  if (Array.isArray(error)) return String(error[1] ?? error[0] ?? 'rejected');
  if (error && typeof error === 'object' && 'message' in error) {
    return String((error as { message: unknown }).message);
  }
  return typeof error === 'string' ? error : 'rejected';
}

// The http url of a service url (ws or wss on the same port).
function httpUrl(url: string): string {
  return url.replace(/^ws/i, 'http');
}

function base64(text: string): string {
  return btoa(String.fromCodePoint(...new TextEncoder().encode(text)));
}

class Miner {
  private state: MinerState = initial;
  private listeners = new Set<() => void>();
  private socket?: WebSocket;
  // Aborts the http requests of a bitcoind connection.
  private abort?: AbortController;
  private template?: Template;
  // A block has been found on the template, so it is not mined further.
  private solved = false;
  private height?: number;
  private renewal?: ReturnType<typeof setInterval>;
  private templates = 0;
  private workers: Worker[] = [];
  private units = new Map<number, Unit>();
  private assigned = new Map<Worker, number>();
  private unitCounter = 0;
  private extranonceCounter = 0;
  private extranonce1 = '';
  private extranonce2Size = 4;
  private job?: Job;
  private target = difficultyTarget(1);
  private requestId = 10;
  private pending = new Map<number, { key: number; sent: number }>();
  // Mining is wanted (started and not stopped), so a lost connection retries.
  private wanted = false;
  // The current run has been subscribed, so a lost connection is not a fault
  // of the settings.
  private subscribed = false;
  private attempts = 0;
  private retry?: ReturnType<typeof setTimeout>;
  private shareKey = 0;
  private channel?: number;
  private standardJobs = new Map<number, { version: number; merkle: Uint8Array }>();
  private previous?: { job: number; previous: Uint8Array; ntime: number; bits: number };
  private standard?: StandardJob;
  private timeCounter = 0;
  private sequence = 0;
  private samples: { time: number; hashes: number }[] = [];
  private counted = 0;
  private timer?: ReturnType<typeof setInterval>;
  private buffer = '';

  subscribe = (listener: () => void) => {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  };

  getSnapshot = () => this.state;

  private update(change: Partial<MinerState>) {
    this.state = { ...this.state, ...change };
    for (const listener of this.listeners) listener();
  }

  start(settings: MinerSettings) {
    this.stop();
    this.update({
      ...initial,
      ...carried(this.state, settings),
      status: 'connecting',
      settings,
      started: Date.now(),
    });
    this.extranonceCounter = 0;
    this.samples = [];
    this.counted = 0;
    this.wanted = true;
    this.subscribed = false;
    this.attempts = 0;
    this.connect(settings);
  }

  private connect(settings: MinerSettings) {
    this.buffer = '';
    this.channel = undefined;
    this.standardJobs.clear();
    this.previous = undefined;
    this.standard = undefined;
    this.template = undefined;
    if (isTemplate(settings.protocol)) {
      this.connectTemplate(settings);
      return;
    }

    let socket: WebSocket;
    try {
      socket = new WebSocket(settings.url);
    } catch (error) {
      this.fail(error instanceof Error ? error.message : 'Invalid stratum URL.');
      return;
    }

    const v2 = settings.protocol === 'v2';
    if (v2) socket.binaryType = 'arraybuffer';
    this.socket = socket;
    socket.onopen = () => {
      if (v2) {
        const url = new URL(settings.url);
        const port = Number(url.port) || (url.protocol === 'wss:' ? 443 : 80);
        socket.send(setupConnection(url.hostname, port, 'libbitcoin-web/1.0'));
        return;
      }

      this.send(SUBSCRIBE, 'mining.subscribe', ['libbitcoin-web/1.0']);
      this.send(AUTHORIZE, 'mining.authorize', [settings.worker, settings.password]);
    };
    socket.onmessage = (event: MessageEvent) => {
      if (typeof event.data === 'string') this.receive(event.data);
      else if (event.data instanceof ArrayBuffer) this.receiveFrame(new Uint8Array(event.data));
    };
    socket.onerror = () => {
      if (this.socket === socket) this.lose(`Cannot connect to ${settings.url}.`);
    };
    socket.onclose = (event) => {
      if (this.socket !== socket) return;
      this.lose(event.reason ? `Disconnected: ${event.reason}` : 'Disconnected by the server.');
    };

    this.clock();
  }

  private clock() {
    if (!this.timer) {
      this.timer = setInterval(() => {
        this.sample();
      }, 1000);
    }
  }

  private connectTemplate(settings: MinerSettings) {
    if (settings.protocol === 'btcd') this.connectBtcd(settings);
    else this.connectBitcoind(settings);
    this.clock();
  }

  // bitcoind templates are long polled over http posts, authorized by basic
  // authentication of each post.
  private connectBitcoind(settings: MinerSettings) {
    const abort = new AbortController();
    this.abort = abort;
    void this.longpoll(settings, abort.signal);
    this.renewal = setInterval(() => {
      void this.call(settings, 'getblocktemplate', [{ rules: ['segwit'] }], abort.signal)
        .then((message) => this.templated(RENEW, message))
        .catch(() => undefined);
    }, REFRESH * 1000);
  }

  private async longpoll(settings: MinerSettings, signal: AbortSignal) {
    let longpollid: string | undefined;
    while (!signal.aborted) {
      let message: Record<string, unknown>;
      try {
        message = await this.call(settings, 'getblocktemplate', [{ rules: ['segwit'], longpollid }], signal);
      } catch (error) {
        if (signal.aborted) return;
        const why = error instanceof Error ? error.message : `Cannot reach ${settings.url}.`;
        if (this.subscribed) this.lose(why);
        else this.fail(why);
        return;
      }

      if (signal.aborted) return;
      const template = this.templated(TEMPLATE, message);
      if (!template) return;
      longpollid = template.longpollid;
    }
  }

  private async call(settings: MinerSettings, method: string, params: unknown[], signal: AbortSignal) {
    const headers: Record<string, string> = { 'Content-Type': 'application/json' };
    if (settings.password) headers.Authorization = `Basic ${base64(`${settings.user ?? ''}:${settings.password}`)}`;
    let response: Response;
    try {
      response = await fetch(httpUrl(settings.url), {
        method: 'POST',
        headers,
        body: JSON.stringify({ jsonrpc: '1.0', id: 1, method, params }),
        signal,
      });
    } catch {
      throw new Error(`Cannot reach ${settings.url}.`);
    }

    if (response.status === 401 || response.status === 403) throw new Error('Not authorized.');
    const message: unknown = await response.json();
    if (!message || typeof message !== 'object') throw new Error('Invalid response.');
    return message as Record<string, unknown>;
  }

  // btcd templates are requested over a websocket, authenticated in band, on
  // each block notification and at an interval.
  private connectBtcd(settings: MinerSettings) {
    let socket: WebSocket;
    try {
      socket = new WebSocket(settings.url);
    } catch (error) {
      this.fail(error instanceof Error ? error.message : 'Invalid service URL.');
      return;
    }

    this.socket = socket;
    socket.onopen = () => {
      if (settings.password) this.request(socket, AUTHENTICATE, 'authenticate', [settings.user ?? '', settings.password]);
      else this.notify(socket);
    };
    socket.onmessage = (event: MessageEvent) => {
      if (typeof event.data === 'string') this.receiveBtcd(socket, event.data);
    };
    socket.onerror = () => {
      if (this.socket === socket) this.lose(`Cannot connect to ${settings.url}.`);
    };
    socket.onclose = (event) => {
      if (this.socket !== socket) return;
      this.lose(event.reason ? `Disconnected: ${event.reason}` : 'Disconnected by the server.');
    };

    this.renewal = setInterval(() => {
      this.request(socket, RENEW, 'getblocktemplate', [{ rules: ['segwit'] }]);
    }, REFRESH * 1000);
  }

  private notify(socket: WebSocket) {
    this.request(socket, NOTIFY, 'notifyblocks', []);
    this.request(socket, TEMPLATE, 'getblocktemplate', [{ rules: ['segwit'] }]);
  }

  private request(socket: WebSocket | undefined, id: number, method: string, params: unknown[]) {
    if (socket?.readyState === WebSocket.OPEN) socket.send(JSON.stringify({ jsonrpc: '1.0', id, method, params }));
  }

  // Each frame is one json-rpc response or notification.
  private receiveBtcd(socket: WebSocket, data: string) {
    let message: unknown;
    try {
      message = JSON.parse(data);
    } catch {
      return;
    }

    if (!message || typeof message !== 'object') return;
    const value = message as Record<string, unknown>;
    if (value.method === 'blockconnected') {
      this.request(socket, RENEW, 'getblocktemplate', [{ rules: ['segwit'] }]);
      return;
    }

    if (value.id === AUTHENTICATE) {
      if (value.error) this.fail(`Not authenticated: ${reason(value.error)}`);
      else this.notify(socket);
      return;
    }

    if (value.id === NOTIFY) return;
    if (value.id === TEMPLATE || value.id === RENEW) {
      this.templated(value.id, value);
      return;
    }

    this.submitted(Number(value.id), value);
  }

  // The template of a response, else the response is a fault of the service.
  private templated(id: number, message: Record<string, unknown>): Template | undefined {
    const template = message.error ? undefined : parseTemplate(message.result);
    if (!template) {
      if (id === RENEW) return undefined;
      const why = `Template refused: ${reason(message.error ?? 'invalid template')}`;
      if (this.subscribed) this.lose(why);
      else this.fail(why);
      return undefined;
    }

    this.setTemplate(template);
    return template;
  }

  // bip22: a submitted block is accepted by a null result, else its reason.
  private submitted(id: number, message: Record<string, unknown>) {
    const key = this.pending.get(id)?.key;
    if (key === undefined) return;
    this.pending.delete(id);
    const { result, error } = message;
    const accepted = !error && result === null;
    const why = typeof result === 'string' ? result : reason(error);
    this.update({
      accepted: this.state.accepted + (accepted ? 1 : 0),
      rejected: this.state.rejected + (accepted ? 0 : 1),
      shares: this.state.shares.map((share) =>
        share.key === key
          ? { ...share, outcome: accepted ? 'accepted' : 'rejected', reason: accepted ? undefined : why }
          : share
      ),
    });
  }

  private submitBlock(id: number, hex: string) {
    const settings = this.state.settings;
    if (settings?.protocol === 'btcd') {
      this.request(this.socket, id, 'submitblock', [hex]);
      return;
    }

    const signal = this.abort?.signal;
    if (!settings || !signal) return;
    void this.call(settings, 'submitblock', [hex], signal)
      .then((message) => {
        this.submitted(id, message);
      })
      .catch((error: unknown) => {
        if (!signal.aborted) this.submitted(id, { error: error instanceof Error ? error.message : 'unanswered' });
      });
  }

  // A template is mined to its block target, as a job of the coinbase that
  // pays the payout script. A renewal of the same work within the refresh
  // interval keeps the current job.
  private setTemplate(template: Template) {
    const settings = this.state.settings;
    const current = this.template;
    if (
      current &&
      current.longpollid === template.longpollid &&
      current.bits === template.bits &&
      current.previous === template.previous &&
      template.time - current.time < REFRESH
    )
      return;

    this.template = template;
    this.solved = false;
    this.extranonce1 = '';
    this.extranonce2Size = EXTRANONCE_SIZE;
    this.subscribed = true;
    this.attempts = 0;
    this.setTarget(bitsTarget(template.bits));
    const id = (++this.templates).toString(16).padStart(8, '0');
    this.setJob(templateJob(template, id, settings?.payout ?? '', settings?.tag ?? ''), template.height);
  }

  stop() {
    this.wanted = false;
    if (this.retry) clearTimeout(this.retry);
    this.retry = undefined;
    this.close();
    this.unanswered();
    this.halt();
    if (this.state.status !== 'stopped' && this.state.status !== 'failed') {
      this.update({ status: 'stopped', message: undefined, hashrate: 0 });
    }
  }

  // Closes the socket or aborts the requests of the connection.
  private close() {
    if (this.renewal) clearInterval(this.renewal);
    this.renewal = undefined;
    const socket = this.socket;
    const abort = this.abort;
    this.socket = undefined;
    this.abort = undefined;
    socket?.close();
    abort?.abort();
  }

  // Shares awaiting an answer are given up on.
  private unanswered() {
    if (this.pending.size === 0) return;
    const keys = new Set([...this.pending.values()].map((entry) => entry.key));
    this.pending.clear();
    this.update({
      shares: this.state.shares.map((share) =>
        keys.has(share.key) ? { ...share, outcome: 'unanswered' } : share
      ),
    });
  }

  // A connection that had worked is retried while mining is wanted, a first
  // connection that fails is a fault of the settings.
  private lose(message: string) {
    if (!this.subscribed || !this.wanted) {
      this.fail(message);
      return;
    }

    this.close();
    this.unanswered();
    this.halt(false);
    const seconds = Math.min(RETRY_CAP, RETRY * 2 ** this.attempts);
    this.attempts += 1;
    this.update({
      status: 'connecting',
      message: `${message} Reconnecting in ${String(seconds)} seconds.`,
      hashrate: 0,
    });
    this.retry = setTimeout(() => {
      this.retry = undefined;
      const settings = this.state.settings;
      if (this.wanted && settings) this.connect(settings);
    }, seconds * 1000);
  }

  // Stops hashing, and the clock unless a reconnect keeps sampling.
  private halt(clock = true) {
    if (clock && this.timer) clearInterval(this.timer);
    if (clock) this.timer = undefined;
    for (const worker of this.workers) worker.terminate();
    this.workers = [];
    this.assigned.clear();
    this.units.clear();
    this.pending.clear();
    this.job = undefined;
  }

  private fail(message: string) {
    this.wanted = false;
    if (this.retry) clearTimeout(this.retry);
    this.retry = undefined;
    this.close();
    this.unanswered();
    this.halt();
    this.update({ status: 'failed', message, hashrate: 0 });
  }

  private send(id: number, method: string, params: unknown[]) {
    this.socket?.send(JSON.stringify({ id, method, params }));
  }

  // Messages may be split across or combined within frames, so complete json
  // values are taken from the text received, the rest kept for more. A json
  // array is a batch of messages.
  private receive(data: string) {
    this.buffer += data;
    for (const text of takeJson(this)) {
      let message: unknown;
      try {
        message = JSON.parse(text);
      } catch {
        continue;
      }
      for (const entry of Array.isArray(message) ? message : [message]) {
        if (entry && typeof entry === 'object') this.handle(entry as Record<string, unknown>);
      }
    }
  }

  // The buffer, for takeJson.
  get text(): string {
    return this.buffer;
  }

  set text(value: string) {
    this.buffer = value.length > 1_000_000 ? '' : value;
  }

  private handle(message: Record<string, unknown>) {
    const method = message.method;
    if (typeof method === 'string') {
      const params = message.params;
      if (method === 'mining.set_difficulty' && Array.isArray(params)) {
        const difficulty = Number(params[0]);
        if (difficulty > 0) this.setDifficulty(difficulty);
      } else if (method === 'mining.notify') {
        const job = parseJob(params);
        if (job) this.setJob(job);
      }
      return;
    }

    const id = Number(message.id);
    const error = message.error;
    if (id === SUBSCRIBE) {
      const result = message.result;
      if (error || !Array.isArray(result) || typeof result[1] !== 'string') {
        this.fail(`Subscribe failed: ${reason(error)}`);
        return;
      }
      this.extranonce1 = result[1];
      this.extranonce2Size = Number(result[2]) || 4;
      const threads = this.state.settings?.threads ?? 1;
      this.send(SUGGEST, 'mining.suggest_difficulty', [suggestedDifficulty(threads)]);
      this.subscribed = true;
      this.attempts = 0;
      if (this.state.status === 'connecting') this.update({ status: 'waiting', message: undefined });
      return;
    }

    if (id === AUTHORIZE) {
      if (error || message.result !== true) this.fail(`Worker not authorized: ${reason(error)}`);
      return;
    }

    const key = this.pending.get(id)?.key;
    if (key === undefined) return;
    this.pending.delete(id);
    const accepted = message.result === true && !error;
    this.update({
      accepted: this.state.accepted + (accepted ? 1 : 0),
      rejected: this.state.rejected + (accepted ? 0 : 1),
      shares: this.state.shares.map((share) =>
        share.key === key
          ? {
              ...share,
              outcome: accepted ? 'accepted' : 'rejected',
              reason: accepted ? undefined : reason(error),
            }
          : share
      ),
    });
  }

  private setDifficulty(difficulty: number) {
    this.setTarget(difficultyTarget(difficulty));
  }

  private setTarget(target: bigint) {
    this.target = target;
    this.update({ difficulty: targetDifficulty(target) });
    const words = targetWords(target);
    for (const worker of this.workers) this.post(worker, { type: 'target', target: words });
  }

  // A stratum v2 frame, one to each binary websocket message.
  private receiveFrame(data: Uint8Array) {
    const message = parseFrame(data);
    if (message) this.handleFrame(message);
  }

  private handleFrame(message: ServerMessage) {
    const settings = this.state.settings;
    switch (message.type) {
      case 'setup_success': {
        const threads = settings?.threads ?? 1;
        const request = ++this.requestId;
        const frame = openStandardMiningChannel(request, settings?.worker ?? '', threads * THREAD_RATE, MAXIMUM_TARGET);
        this.socket?.send(frame);
        return;
      }
      case 'setup_error':
        this.fail(`Setup refused: ${message.code}`);
        return;
      case 'open_success':
        this.channel = message.channel;
        this.setTarget(message.target);
        this.subscribed = true;
        this.attempts = 0;
        if (this.state.status === 'connecting') this.update({ status: 'waiting', message: undefined });
        return;
      case 'open_error':
        this.fail(`Worker not authorized: ${message.code}`);
        return;
      case 'job':
        if (message.channel !== this.channel) return;
        this.standardJobs.set(message.job, { version: message.version, merkle: message.merkle });
        if (message.ntime !== undefined && this.previous) this.activate(message.job, message.ntime);
        return;
      case 'prev_hash':
        if (message.channel !== this.channel) return;
        this.previous = { job: message.job, previous: message.previous, ntime: message.ntime, bits: message.bits };
        this.activate(message.job, message.ntime);
        return;
      case 'target':
        if (message.channel === this.channel) this.setTarget(message.target);
        return;
      case 'shares_success':
        this.answer((sequence) => sequence <= message.last, true);
        return;
      case 'shares_error':
        this.answer((sequence) => sequence === message.sequence, false, message.code);
        return;
      case 'close':
        if (message.channel === this.channel) this.lose(`Channel closed: ${message.code}`);
        return;
      default:
        return;
    }
  }

  // Shares pending by sequence are answered.
  private answer(matches: (sequence: number) => boolean, accepted: boolean, why?: string) {
    const keys = new Set<number>();
    for (const [sequence, entry] of this.pending) {
      if (!matches(sequence)) continue;
      keys.add(entry.key);
      this.pending.delete(sequence);
    }

    if (keys.size === 0) return;
    this.update({
      accepted: this.state.accepted + (accepted ? keys.size : 0),
      rejected: this.state.rejected + (accepted ? 0 : keys.size),
      shares: this.state.shares.map((share) =>
        keys.has(share.key)
          ? { ...share, outcome: accepted ? 'accepted' : 'rejected', reason: accepted ? undefined : why }
          : share
      ),
    });
  }

  // The standard job becomes current with its previous block.
  private activate(job: number, ntime: number) {
    const known = this.standardJobs.get(job);
    const previous = this.previous;
    if (!known || !previous) return;
    this.standard = { job, version: known.version, merkle: known.merkle, previous: previous.previous, ntime, bits: previous.bits };
    this.timeCounter = 0;
    this.update({
      status: 'mining',
      jobs: this.state.jobs + 1,
      job: {
        id: job.toString(16).padStart(8, '0'),
        previous: bytesToHex(previous.previous.slice().reverse()),
        networkDifficulty: targetDifficulty(bitsTarget(bitsHex(previous.bits))),
        time: ntime,
        clean: true,
        received: Date.now(),
      },
    });

    const threads = this.state.settings?.threads ?? 1;
    while (this.workers.length < threads) this.spawn();
    for (const worker of this.workers) this.assign(worker);
  }

  // The height of the standard job, which its messages do not carry, from
  // its previous block.
  setHeight(previous: string, height: number) {
    const standard = this.standard;
    const job = this.state.job;
    if (!standard || !job || job.previous !== previous || job.height !== undefined) return;
    this.standard = { ...standard, height };
    for (const unit of this.units.values()) {
      if (unit.standard?.job === standard.job) unit.height = height;
    }

    this.update({
      job: { ...job, height },
      shares: this.state.shares.map((share) =>
        share.job === job.id && share.height === undefined ? { ...share, height } : share
      ),
    });
  }

  private setJob(job: Job, height = coinbaseHeight(job)) {
    this.job = job;
    this.height = height;
    this.update({
      status: 'mining',
      jobs: this.state.jobs + 1,
      job: {
        id: job.id,
        height,
        tag: coinbaseTag(job),
        previous: previousHash(job),
        networkDifficulty: targetDifficulty(bitsTarget(job.bits)),
        time: Number.parseInt(job.time, 16),
        clean: job.clean,
        received: Date.now(),
      },
    });

    const threads = this.state.settings?.threads ?? 1;
    while (this.workers.length < threads) this.spawn();

    // Every worker moves to the latest job.
    for (const worker of this.workers) this.assign(worker);
  }

  private spawn() {
    const worker = new MinerWorker();
    worker.onmessage = (event: MessageEvent<WorkerResponse>) => {
      this.fromWorker(worker, event.data);
    };
    this.workers.push(worker);
  }

  private post(worker: Worker, request: WorkerRequest) {
    worker.postMessage(request);
  }

  // A unit of a standard job is its nonce space at a later time than the last.
  private unitOf(): Unit | undefined {
    const standard = this.standard;
    if (standard) {
      const ntime = standard.ntime + this.timeCounter++;
      return {
        header: standardHeader(standard.version, standard.previous, standard.merkle, ntime, standard.bits),
        id: standard.job.toString(16).padStart(8, '0'),
        bits: bitsHex(standard.bits),
        height: standard.height,
        standard: { ...standard, ntime },
      };
    }

    const job = this.job;
    if (!job || this.solved) return undefined;
    const extranonce2 = extranonce(this.extranonceCounter++, this.extranonce2Size);
    return {
      header: buildHeader(job, this.extranonce1, extranonce2),
      id: job.id,
      bits: job.bits,
      height: this.height,
      job,
      extranonce2,
      template: this.template,
    };
  }

  private assign(worker: Worker) {
    const value = this.unitOf();
    if (!value) return;
    const previous = this.assigned.get(worker);
    if (previous !== undefined) this.units.delete(previous);

    const header = value.header;
    const unit = ++this.unitCounter;
    this.units.set(unit, value);
    this.assigned.set(worker, unit);
    this.post(worker, {
      type: 'work',
      unit,
      work: headerWork(header),
      target: targetWords(this.target),
      start: 0,
      end: NONCES,
    });
  }

  private fromWorker(worker: Worker, response: WorkerResponse) {
    if (response.type === 'progress') {
      this.counted += response.hashes;
      if (this.state.simd !== response.simd) this.update({ simd: response.simd });
      return;
    }

    const unit = this.units.get(response.unit);
    if (!unit) return;
    if (response.type === 'done') {
      this.assign(worker);
      return;
    }

    this.found(unit, response.nonce);
  }

  private found(unit: Unit, nonce: number) {
    const header = withNonce(unit.header, nonce);
    const value = hashValue(header);

    // Checked here too, as the target may have changed since.
    if (value > this.target) return;
    const block = value <= bitsTarget(unit.bits);

    // The workers stop on the first block of a template, until the next.
    if (unit.template) {
      if (unit.template !== this.template || this.solved) return;
      this.solved = true;
      for (const worker of this.workers) this.post(worker, { type: 'stop' });
      this.units.clear();
      this.assigned.clear();
    }

    const key = ++this.shareKey;
    const standard = unit.standard;
    if (unit.template && unit.job && unit.extranonce2 !== undefined) {
      const id = ++this.requestId;
      this.pending.set(id, { key, sent: Date.now() });
      this.submitBlock(id, blockHex(header, unit.job, unit.extranonce2, unit.template));
    } else if (standard && this.channel !== undefined) {
      const sequence = ++this.sequence;
      this.pending.set(sequence, { key, sent: Date.now() });
      this.socket?.send(submitSharesStandard(this.channel, sequence, standard.job, nonce, standard.ntime, standard.version));
    } else if (unit.job && unit.extranonce2 !== undefined) {
      const id = ++this.requestId;
      this.pending.set(id, { key, sent: Date.now() });
      this.send(id, 'mining.submit', submitParams(this.state.settings?.worker ?? '', unit.job, unit.extranonce2, nonce));
    } else {
      return;
    }

    const share: Share = {
      key,
      time: Date.now(),
      job: unit.id,
      height: unit.height,
      nonce: (nonce >>> 0).toString(16).padStart(8, '0'),
      hash: headerHash(header),
      difficulty: targetDifficulty(value),
      block,
      outcome: 'pending',
    };
    this.update({
      blocks: this.state.blocks + (block ? 1 : 0),
      shares: [share, ...this.state.shares].slice(0, SHARES),
    });
  }

  private sample() {
    const time = Date.now();
    for (const entry of this.pending.values()) {
      if (time - entry.sent > ANSWER * 1000) {
        this.lose(`No answer from the service for ${String(ANSWER)} seconds.`);
        return;
      }
    }

    this.samples.push({ time, hashes: this.counted });
    while (this.samples.length > WINDOW + 1) this.samples.shift();
    const first = this.samples[0];
    const seconds = (time - first.time) / 1000;
    const hashrate = seconds > 0 ? (this.counted - first.hashes) / seconds : 0;
    const history = [...this.state.history, { time, rate: hashrate }].slice(-HISTORY);
    this.update({ hashrate, history, hashes: this.counted });
  }
}

export const miner = new Miner();

// Takes the complete json values (objects or arrays) from the text of the
// holder, leaving any incomplete value as its text.
export function takeJson(holder: { text: string }): string[] {
  const text = holder.text;
  const values: string[] = [];
  let depth = 0;
  let start = -1;
  let quoted = false;
  let escaped = false;
  let consumed = 0;
  for (let index = 0; index < text.length; index++) {
    const character = text[index];
    if (quoted) {
      if (escaped) escaped = false;
      else if (character === '\\') escaped = true;
      else if (character === '"') quoted = false;
      continue;
    }

    if (character === '"') {
      quoted = depth > 0;
    } else if (character === '{' || character === '[') {
      if (depth === 0) start = index;
      depth++;
    } else if (character === '}' || character === ']') {
      if (depth === 0) {
        consumed = index + 1;
        continue;
      }
      depth--;
      if (depth === 0) {
        values.push(text.slice(start, index + 1));
        consumed = index + 1;
      }
    } else if (depth === 0) {
      consumed = index + 1;
    }
  }

  holder.text = text.slice(consumed);
  return values;
}
