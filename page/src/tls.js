// SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
// SPDX-License-Identifier: GPL-3.0-only

/**
 * @file tls.js
 * @description Settings > TLS: HTTPS on/off, the certificate the device serves, creating a
 * new self-signed certificate, installing your own, and OpenSSL examples for making one.
 */

import {getAuthHeaders, handleResponse} from './api.js';

const $ = (id) => document.getElementById(id);
let current = null;
let bound = false;

async function fetchTls() {
    const response = await fetch('/api/tls', {headers: getAuthHeaders()});
    return (await handleResponse(response)).json();
}

async function postTls(body) {
    const response = await fetch('/api/tls', {
        method: 'POST',
        headers: {'Content-Type': 'application/json', ...getAuthHeaders()},
        body: JSON.stringify(body),
    });
    return (await handleResponse(response)).json();
}

function isIp(host) {
    return /^\d{1,3}(\.\d{1,3}){3}$/.test(host);
}

/** The names the examples put in subjectAltName: the .local name and the address in use. */
function exampleNames(cert) {
    const names = cert?.names ?? [];
    const dns = names.find((n) => !isIp(n)) ?? `${location.hostname}`;
    const ips = names.filter((n) => isIp(n) && n !== '192.168.4.1');
    if (isIp(location.hostname) && !ips.includes(location.hostname)) ips.unshift(location.hostname);
    const cn = dns.replace(/\.local$/, '');
    const san = [`DNS:${dns}`, ...ips.map((ip) => `IP:${ip}`)].join(',');
    return {cn, san};
}

function renderExamples(cert) {
    const {cn, san} = exampleNames(cert);
    $('tls-openssl-self').textContent =
        `openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \\
  -keyout powermate.key -out powermate.crt -days 3650 \\
  -subj "/CN=${cn}" \\
  -addext "subjectAltName=${san}"`;
    $('tls-openssl-ca').textContent =
        `# once: your own CA
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \\
  -keyout ca.key -out ca.crt -days 3650 -subj "/CN=PowerMate Local CA"

# per device: key, request, certificate signed by the CA
openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \\
  -keyout powermate.key -out powermate.csr -subj "/CN=${cn}"
printf "subjectAltName=${san}\\nextendedKeyUsage=serverAuth\\n" > powermate.ext
openssl x509 -req -in powermate.csr -CA ca.crt -CAkey ca.key -CAcreateserial \\
  -days 825 -extfile powermate.ext -out powermate.crt

# install powermate.crt and powermate.key here`;
}

function render(data) {
    current = data;
    const cert = data.certificate ?? {};
    $('https-toggle').checked = data.https_enabled === true;
    $('tls-cert-source').textContent = cert.source === 'user' ? 'installed' : cert.source ? 'self-signed' : 'none';
    $('tls-cert-subject').textContent = cert.subject ?? '-';
    $('tls-cert-issuer').textContent = cert.issuer ?? '-';
    $('tls-cert-names').textContent = cert.names?.length ? cert.names.join(', ') : '-';
    $('tls-cert-valid').textContent = cert.not_before ? `${cert.not_before} – ${cert.not_after}` : '-';
    $('tls-cert-key').textContent = cert.key ? `${cert.key}${cert.chain_length > 1 ? `, chain of ${cert.chain_length}` : ''}` : '-';
    $('tls-cert-fingerprint').textContent = cert.sha256 ?? '-';

    let note = '';
    if (data.https_enabled && !data.https_active) note = 'HTTPS is turned on but did not start; the device serves HTTP.';
    else if (data.restart_required) note = 'The new certificate is served after the next reboot.';
    $('tls-cert-note').textContent = note;
    $('tls-plain-http-warning').hidden = location.protocol === 'https:';
    renderExamples(cert);
}

async function withBusy(button, label, fn) {
    const text = button.innerHTML;
    button.disabled = true;
    button.innerHTML = `<span class="spinner-border spinner-border-sm" aria-hidden="true"></span> ${label}`;
    try {
        return await fn();
    } catch (error) {
        alert(error.message);
    } finally {
        button.disabled = false;
        button.innerHTML = text;
    }
}

async function applyHttps() {
    const on = $('https-toggle').checked;
    if (current && on === current.https_enabled) return;
    const target = `${on ? 'https' : 'http'}://${location.hostname}/`;
    const message = on
        ? `The device reboots and serves the Web UI at ${target}.\n\n` +
          'With the self-signed certificate the browser warns once; check that the SHA-256 ' +
          'fingerprint it shows matches the one listed here before you continue.'
        : `The device reboots and serves the Web UI at ${target} without encryption.`;
    if (!confirm(message)) {
        $('https-toggle').checked = !on;
        return;
    }
    const done = await withBusy($('https-apply-button'), 'Rebooting...', async () => {
        const data = await postTls({https_enabled: on});
        render(data);
        // The device reboots (~3 s) and starts its server (~5 s more).
        if (data.reboot) setTimeout(() => location.assign(target), 12000);
        return true;
    });
    if (!done) $('https-toggle').checked = current?.https_enabled === true;
}

async function generate() {
    if (!confirm('Replace the current certificate with a new self-signed one? ' +
        'Browsers and clients that trusted the old one will warn again.')) return;
    await withBusy($('tls-generate-button'), 'Creating...', async () => render(await postTls({action: 'generate'})));
}

async function upload() {
    const cert = $('tls-cert-input').value.trim();
    const key = $('tls-key-input').value.trim();
    if (!cert.includes('BEGIN CERTIFICATE') || !key.includes('PRIVATE KEY')) {
        alert('Paste or choose both the certificate and the private key (PEM).');
        return;
    }
    await withBusy($('tls-upload-button'), 'Installing...', async () => {
        render(await postTls({cert: `${cert}\n`, key: `${key}\n`}));
        $('tls-key-input').value = '';
        $('tls-key-file').value = '';
        alert(current.https_active ? 'Certificate installed. It is served after the next reboot.' : 'Certificate installed.');
    });
}

function readInto(fileInput, textarea) {
    fileInput.addEventListener('change', async () => {
        const file = fileInput.files?.[0];
        if (file) textarea.value = await file.text();
    });
}

/** Loads the HTTPS state when the settings dialog opens; binds the controls once. */
export async function initTlsSettings() {
    if (!$('tls-settings')) return;
    if (!bound) {
        bound = true;
        $('https-apply-button').addEventListener('click', applyHttps);
        $('tls-generate-button').addEventListener('click', generate);
        $('tls-upload-button').addEventListener('click', upload);
        readInto($('tls-cert-file'), $('tls-cert-input'));
        readInto($('tls-key-file'), $('tls-key-input'));
    }
    try {
        render(await fetchTls());
    } catch (error) {
        console.error('Error loading HTTPS settings:', error);
    }
}
