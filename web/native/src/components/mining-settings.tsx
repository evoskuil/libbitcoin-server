import { useEffect, useRef, useState, type FormEvent, type ReactNode } from 'react';
import type { NetworkParams } from '@/lib/api/client';
import { isAddress } from '@/lib/api/providers/libbitcoin-mapper';
import type { MinerProtocol, MinerSettings } from '@/lib/mining/miner';
import { closeOnBackdrop } from '@/lib/utils/dialog';

/** The service and credentials of a miner protocol. */
export interface ServiceSettings {
  url: string;
  worker: string;
  password: string;
}

/** Miner settings of the page, with the payout fields the node may supply.
 * The url, worker and password are those of the protocol, which are kept
 * apart from those of each other protocol. */
export interface PageSettings extends MinerSettings {
  address: string;
  tag: string;
  services: Partial<Record<MinerProtocol, ServiceSettings>>;
}

/** The service settings of a protocol never configured. */
export const DEFAULT_SERVICE: ServiceSettings = { url: '', worker: 'browser', password: '' };

/** The settings with their service settings kept, and those of the protocol. */
export function withProtocol(settings: PageSettings, protocol: MinerProtocol): PageSettings {
  const services = {
    ...settings.services,
    [settings.protocol ?? 'v1']: { url: settings.url, worker: settings.worker, password: settings.password },
  };
  return { ...settings, ...(services[protocol] ?? DEFAULT_SERVICE), services, protocol };
}

/** The fields of a protocol: whether its service url is entered, and the
 * payout address and coinbase tag the node advertises (not entered). */
export interface ProtocolFields {
  url: boolean;
  payout?: string;
  tag?: string;
}

const PROTOCOLS: Record<MinerProtocol, string> = {
  v1: 'Stratum v1',
  v2: 'Stratum v2',
  bitcoind: 'GBT (bitcoind)',
  btcd: 'GBT (btcd)',
};

/** The name of a miner protocol. */
export function protocolName(protocol: MinerProtocol = 'v1'): string {
  return PROTOCOLS[protocol];
}

const INPUT =
  'border-accent/40 bg-surface text-textPrimary placeholder:text-textMuted focus:border-accent focus:ring-accent w-full rounded-md border px-3 py-1.5 text-sm';

// The miner settings, edited in a modal dialog and applied on save.
export function MiningSettings({
  open,
  settings,
  require,
  wallet,
  network,
  maximum,
  protocols,
  onSave,
  onClose,
}: {
  open: boolean;
  settings: PageSettings;
  require: (protocol: MinerProtocol) => ProtocolFields;
  protocols: MinerProtocol[];
  wallet?: NetworkParams;
  network?: string;
  maximum: number;
  onSave: (settings: PageSettings) => void;
  onClose: () => void;
}) {
  const dialog = useRef<HTMLDialogElement>(null);
  const [draft, setDraft] = useState(settings);

  useEffect(() => {
    const element = dialog.current;
    if (!element) return;
    if (open && !element.open) {
      setDraft(settings);
      element.showModal();
    } else if (!open && element.open) element.close();
  }, [open, settings]);

  const change = <Key extends keyof PageSettings>(key: Key, value: PageSettings[Key]) => {
    setDraft((current) => ({ ...current, [key]: value }));
  };

  // The username is the whole identity where the node pays its own address.
  const fields = require(draft.protocol ?? 'v1');
  const username = fields.payout !== undefined;
  const wrongAddress =
    !fields.payout && wallet !== undefined && draft.address.trim() !== '' && !isAddress(draft.address, wallet);

  const missing =
    wrongAddress ||
    (fields.url && !draft.url.trim()) ||
    (!fields.payout && !draft.address.trim()) ||
    (!fields.tag && !draft.tag.trim()) ||
    (username && !draft.worker.trim());

  const onSubmit = (event: FormEvent) => {
    event.preventDefault();
    const trimmed = {
      ...draft,
      url: draft.url.trim(),
      worker: draft.worker.trim(),
      address: draft.address.trim(),
      tag: draft.tag.trim(),
    };
    onSave(withProtocol(trimmed, trimmed.protocol ?? 'v1'));
  };

  return (
    <dialog
      ref={dialog}
      aria-labelledby="mining-settings-title"
      onClose={onClose}
      onCancel={onClose}
      onMouseDown={closeOnBackdrop(onClose)}
      className="border-accent/40 bg-surface text-textPrimary m-auto w-[min(32rem,calc(100vw-2rem))] rounded-2xl border p-0 shadow-2xl backdrop:bg-black/60"
    >
      <form onSubmit={onSubmit} className="space-y-4 p-6">
        <h2 id="mining-settings-title" className="text-accent text-lg font-semibold">
          Settings
        </h2>
        {protocols.length > 1 && (
          <Field label="Protocol">
            <select
              className={INPUT}
              value={draft.protocol ?? 'v1'}
              onChange={(event) => {
                const protocol = protocols.find((value) => value === event.target.value) ?? 'v1';
                setDraft((current) => withProtocol(current, protocol));
              }}
            >
              {protocols.map((value) => (
                <option key={value} value={value}>
                  {protocolName(value)}
                </option>
              ))}
            </select>
          </Field>
        )}
        {fields.url && (
          <Field label="Service">
            <input
              className={INPUT}
              value={draft.url}
              spellCheck={false}
              placeholder="ws://host:port"
              onChange={(event) => {
                change('url', event.target.value);
              }}
            />
          </Field>
        )}
        <Field label="Payout address">
          <input
            className={`${INPUT} disabled:opacity-60`}
            value={fields.payout ?? draft.address}
            disabled={fields.payout !== undefined}
            spellCheck={false}
            onChange={(event) => {
              change('address', event.target.value);
            }}
          />
          {wrongAddress && <span className="block text-xs text-rose-300">Not a {network ?? 'node'} address.</span>}
        </Field>
        <Field label="Coinbase tag">
          <input
            className={`${INPUT} disabled:opacity-60`}
            value={fields.tag ?? draft.tag}
            disabled={fields.tag !== undefined}
            spellCheck={false}
            onChange={(event) => {
              change('tag', event.target.value);
            }}
          />
        </Field>
        <div className="grid gap-4 sm:grid-cols-[minmax(0,1fr)_7rem]">
          <Field label="Username">
            <input
              className={INPUT}
              value={draft.worker}
              spellCheck={false}
              placeholder={username ? undefined : 'optional'}
              onChange={(event) => {
                change('worker', event.target.value);
              }}
            />
          </Field>
          <Field label="Threads">
            <input
              className={INPUT}
              type="number"
              min={1}
              max={maximum}
              value={draft.threads}
              onChange={(event) => {
                change('threads', Math.min(maximum, Math.max(1, Math.floor(Number(event.target.value) || 1))));
              }}
            />
          </Field>
        </div>
        <Field label="Password">
          <input
            className={INPUT}
            type="password"
            value={draft.password}
            autoComplete="off"
            placeholder="optional"
            onChange={(event) => {
              change('password', event.target.value);
            }}
          />
        </Field>
        <div className="flex justify-end gap-3 pt-2">
          <button
            type="button"
            onClick={onClose}
            className="border-accent/40 text-textPrimary hover:border-accent hover:text-accent rounded-md border px-4 py-1.5 text-sm"
          >
            Cancel
          </button>
          <button
            type="submit"
            disabled={missing}
            className="border-accent bg-accent/20 text-accent hover:bg-accent/30 rounded-md border px-4 py-1.5 text-sm font-medium disabled:cursor-not-allowed disabled:opacity-50"
          >
            Save
          </button>
        </div>
      </form>
    </dialog>
  );
}

function Field({ label, children }: { label: string; children: ReactNode }) {
  return (
    <label className="block space-y-1">
      <span className="text-accentSecondary text-xs tracking-wide uppercase">{label}</span>
      {children}
    </label>
  );
}
