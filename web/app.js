(() => {
  'use strict';

  const $ = (id) => document.getElementById(id);
  const isRecord = (value) => value !== null && typeof value === 'object' && !Array.isArray(value);
  const record = (value) => isRecord(value) ? value : {};
  const number = (value) => typeof value === 'number' && Number.isFinite(value) && Math.abs(value) <= Number.MAX_SAFE_INTEGER ? value : null;
  const integer = (value) => Number.isSafeInteger(value) ? value : null;
  const count = (value) => integer(value) !== null && value >= 0 ? value : null;
  const text = (value, fallback = '—') => typeof value === 'string' ? value : (number(value) !== null ? String(value) : fallback);
  const formatNumber = (value) => number(value) === null ? '—' : value.toLocaleString();
  const fixed = (value, digits) => number(value) === null || value < 0 ? '—' : value.toFixed(digits);
  const state = {
    labels: [], throughput: [], queue: [], active: [], previousAccounts: new Map(),
    action: null, page: 1, limit: 50, hasNext: false, accountsRequest: 0,
    transactionsRequest: 0, accountsLoading: false, transactionsLoading: false,
    transactionSignature: '', tableTimer: null, stopped: false
  };
  const reasonLabels = ['INSUFFICIENT_FUNDS', 'ACCOUNT_NOT_FOUND', 'ACCOUNT_NOT_ACTIVE', 'INVALID_AMOUNT', 'SAME_ACCOUNT', 'DB_DEADLOCK_EXHAUSTED', 'DB_ERROR', 'QUEUE_CLOSED', 'WORKER_ERROR', 'OTHER'];
  const invariants = [
    ['I1', 'Total balance conserved'], ['I2', 'No negative balances'],
    ['I3', 'Exactly one final status per request'], ['I4', 'No unresolved PENDING entries'],
    ['I5', 'Ledger matches account balances']
  ];
  const charts = {};

  function element(tag, content, className = '') {
    const node = document.createElement(tag);
    if (content !== undefined) node.textContent = text(content, '');
    if (className) node.className = className;
    return node;
  }

  function showToast(message, error = false) {
    const toast = element('div', message, `toast${error ? ' error' : ''}`);
    $('toast-region').append(toast);
    window.setTimeout(() => toast.remove(), 5000);
  }

  function setResult(id, message, kind = '') {
    const node = $(id);
    node.textContent = message;
    node.className = `result-box${id === 'demo-result' ? ' demo-result' : ''}${kind ? ` ${kind}` : ''}`;
  }

  function panel(title, message, kind = '') {
    const node = $('demo-result');
    node.className = `result-box demo-result${kind ? ` ${kind}` : ''}`;
    node.replaceChildren(element('h3', title), element('p', message));
    return node;
  }

  function details(parent, entries) {
    const list = element('dl', undefined, 'result-details');
    entries.forEach(([label, value]) => list.append(element('dt', label), element('dd', value)));
    parent.append(list);
  }

  function messages(parent, values) {
    if (!Array.isArray(values)) return;
    const list = element('ul', undefined, 'result-messages');
    values.filter((value) => typeof value === 'string').slice(0, 50).forEach((value) => list.append(element('li', value)));
    if (list.childElementCount) parent.append(list);
  }

  function beginAction(kind, message) {
    const action = { kind, accepted: false, ready: false, finished: false, interrupted: false, signature: '' };
    state.action = action;
    panel(kind.replace(/_/g, ' '), message);
    $('demo-result').focus();
    return action;
  }

  async function api(path, options = {}) {
    const controller = new AbortController();
    const timeout = window.setTimeout(() => controller.abort(), options.method === 'POST' ? 60000 : 10000);
    try {
      const response = await fetch(path, {
        ...options, cache: 'no-store', signal: controller.signal,
        headers: { 'Content-Type': 'application/json', ...options.headers }
      });
      let body;
      try { body = await response.json(); } catch (_) { throw new Error(`Invalid JSON response (${response.status})`); }
      if (!isRecord(body)) throw new Error(`Invalid API response (${response.status}): expected an object`);
      if (!response.ok) {
        const error = new Error(text(record(body.error).reason, text(body.reason, `HTTP ${response.status}`)));
        error.body = body;
        error.status = response.status;
        throw error;
      }
      return body;
    } catch (error) {
      if (error.name === 'AbortError') throw new Error('Request timed out. Refresh the result before retrying a command.');
      throw error;
    } finally {
      window.clearTimeout(timeout);
    }
  }

  const post = (path, body = {}) => api(path, { method: 'POST', body: JSON.stringify(body) });

  function drawCharts() {
    if (typeof window.Chart !== 'function') return;
    const common = {
      animation: false, responsive: true, maintainAspectRatio: false,
      plugins: { legend: { labels: { color: '#9ab0c0', boxWidth: 10, font: { size: 10 } } } },
      scales: {
        x: { ticks: { color: '#718696', maxTicksLimit: 8 }, grid: { color: '#20303d' } },
        y: { ticks: { color: '#718696' }, grid: { color: '#20303d' } }
      }
    };
    try {
      charts.throughput = new window.Chart($('throughput-chart'), { type: 'line', data: { labels: [], datasets: [{ label: 'tx/s', data: [], borderColor: '#36d6e7', backgroundColor: '#36d6e722', fill: true, tension: .3, pointRadius: 0 }] }, options: common });
      charts.outcomes = new window.Chart($('outcome-chart'), { type: 'bar', data: { labels: ['Committed', 'Rolled back', 'Retries'], datasets: [{ data: [], backgroundColor: ['#43e09a', '#ff657c', '#ffc857'], borderRadius: 4 }] }, options: { ...common, plugins: { legend: { display: false } } } });
      charts.reasons = new window.Chart($('reason-chart'), { type: 'doughnut', data: { labels: reasonLabels, datasets: [{ data: [], backgroundColor: ['#ff657c', '#ff9365', '#ffc857', '#a879ff', '#ff78b5', '#6c9cff', '#36d6e7', '#7f93a5', '#ca7cff', '#526473'], borderWidth: 0 }] }, options: { ...common, cutout: '63%', scales: {} } });
      charts.queue = new window.Chart($('queue-chart'), { type: 'line', data: { labels: [], datasets: [{ label: 'Queue', data: [], borderColor: '#ffc857', tension: .3, pointRadius: 0 }, { label: 'Active', data: [], borderColor: '#6c9cff', tension: .3, pointRadius: 0 }] }, options: common });
    } catch (_) {
      showToast('Charts unavailable; live values and result panels remain available.', true);
    }
  }

  function updateChart(name, labels, values) {
    const chart = charts[name];
    if (!chart) return;
    try {
      if (labels) chart.data.labels = labels;
      values.forEach((data, index) => { chart.data.datasets[index].data = data; });
      chart.update();
    } catch (_) { delete charts[name]; }
  }

  function checkOutcome(value) {
    const report = record(value);
    if (report.passed === false || invariants.some(([key]) => report[key] === false)) return 'error';
    return report.passed === true && invariants.every(([key]) => report[key] === true) ? 'success' : 'unknown';
  }

  function renderCheckBadge(report) {
    const outcome = checkOutcome(report);
    $('invariant-badge').textContent = outcome === 'success' ? 'INVARIANTS OK' : outcome === 'error' ? 'INVARIANTS VIOLATED' : 'CHECK UNKNOWN';
    $('invariant-badge').className = `invariant-badge ${outcome === 'success' ? 'ok' : outcome === 'error' ? 'bad' : 'unknown'}`;
  }

  function appendCheck(parent, value) {
    const report = record(value);
    const outcome = checkOutcome(report);
    const section = element('section', undefined, 'check-results');
    section.append(element('h3', 'Consistency check'), element('p', outcome === 'success' ? 'PASS — all five invariants verified.' : outcome === 'error' ? 'FAIL — consistency was not verified.' : 'UNKNOWN — a complete boolean checklist was not supplied.'));
    const list = element('ul', undefined, 'invariant-list');
    invariants.forEach(([key, label]) => {
      const status = report[key] === true ? 'PASS' : report[key] === false ? 'FAIL' : 'UNKNOWN';
      const row = element('li');
      row.append(element('span', `${key} · ${label}`), element('strong', status, status === 'PASS' ? 'status-committed' : status === 'FAIL' ? 'status-rolled' : 'status-pending'));
      list.append(row);
    });
    section.append(list);
    if (integer(report.expected_total_minor) !== null || integer(report.actual_total_minor) !== null) {
      details(section, [['Expected total (minor)', formatNumber(integer(report.expected_total_minor))], ['Actual total (minor)', formatNumber(integer(report.actual_total_minor))]]);
    }
    messages(section, report.violations);
    parent.append(section);
    return outcome;
  }

  function recoveryOutcome(value) {
    const outer = record(value);
    const report = isRecord(outer.recovery) ? outer.recovery : outer;
    if (outer.succeeded === false || report.succeeded === false || count(report.remaining_pending) > 0 || (Array.isArray(report.errors) && report.errors.length > 0)) return 'error';
    return (outer.succeeded === true || report.succeeded === true) && count(report.remaining_pending) === 0 ? 'success' : 'unknown';
  }

  function renderRecovery(value, crash = false, requestError = null) {
    const outer = record(value);
    const report = isRecord(outer.recovery) ? outer.recovery : outer;
    const outcome = requestError ? 'error' : recoveryOutcome(outer);
    const node = panel(crash ? 'Crash + recover result' : 'Recovery result', requestError ? `Request failed (HTTP ${requestError.status}): ${requestError.message}. Reported metrics follow.` : outcome === 'success' ? 'Recovery succeeded; no PENDING entries remain.' : outcome === 'error' ? 'Recovery failed or left unresolved entries.' : 'Recovery completion could not be verified.', outcome);
    details(node, [
      ['PENDING seen', formatNumber(count(report.pending_seen))], ['Resolved as committed', formatNumber(count(report.committed))],
      ['Resolved as rolled back', formatNumber(count(report.rolled_back))], ['Remaining PENDING', formatNumber(count(report.remaining_pending))]
    ]);
    if (typeof outer.reason === 'string') node.append(element('p', outer.reason));
    messages(node, report.errors);
    if (crash || Object.hasOwn(outer, 'check')) {
      const check = appendCheck(node, outer.check);
      if (check === 'error' || outcome === 'error') node.className = 'result-box demo-result error';
      else if (check !== 'success' || outcome !== 'success') node.className = 'result-box demo-result unknown';
      if (isRecord(outer.check)) renderCheckBadge(outer.check);
    }
  }

  function demoMatches(data, action) {
    if (action.kind === 'DEADLOCK') return data.scenario === 'opposing';
    return data.scenario === 'deposit' && data.mode === (action.kind === 'RACE' ? 'unsafe' : 'safe');
  }

  function renderDemo(data, freshAction) {
    const action = state.action;
    if (!action || !action.accepted || action.finished || !['RACE', 'SAFE', 'DEADLOCK'].includes(action.kind)) return;
    if (!demoMatches(data, action)) return;
    // The 202 acknowledges a new run. A post-ack read also covers demos that
    // finish before the first running event; old idle deposit snapshots cannot arm it.
    if (!action.ready && freshAction !== action && data.running !== true) return;
    action.ready = true;
    const supplied = data.demo_result !== null && data.demo_result !== undefined;
    if (supplied && !isRecord(data.demo_result)) {
      if (action.signature !== 'invalid-result') panel('Demo result unavailable', 'Invalid demo_result fields; completion and balance match are not verified.', 'unknown');
      action.signature = 'invalid-result';
      return;
    }
    const result = record(data.demo_result);
    if (supplied && text(result.kind, '').toUpperCase() !== action.kind) return;
    const finished = data.running === false && (!supplied || result.finished === true);
    const expected = integer(supplied ? result.expected_minor : data.expected_demo_balance_minor);
    const actual = integer(supplied ? result.actual_minor : data.actual_demo_balance_minor);
    const committed = count(supplied ? result.committed : data.committed);
    const rolledBack = count(supplied ? result.rolled_back : data.rolled_back);
    const difference = expected !== null && actual !== null ? integer(expected - actual) : null;
    const reportedLost = integer(result.lost_minor);
    const inconsistent = supplied && reportedLost !== null && difference !== null && reportedLost !== difference;
    const signature = JSON.stringify([finished, text(result.finished, ''), expected, actual, committed, rolledBack, reportedLost, text(result.reason, ''), action.interrupted]);
    if (signature === action.signature) return;
    action.signature = signature;
    let message = 'Running — balance comparison is pending completion.';
    let outcome = '';
    if (action.interrupted || (typeof result.reason === 'string' && result.reason)) {
      message = 'Stopped or failed — a complete demo result is not verified.';
      outcome = 'error';
    }
    else if (finished && action.kind === 'DEADLOCK') message = 'Opposing-transfer workload finished. Run a consistency check to verify invariants.';
    else if (finished && inconsistent) { message = 'Result fields disagree; the balance comparison is not verified.'; outcome = 'error'; }
    else if (finished && difference === 0) {
      message = 'Completed — expected and actual balances match; no lost minor units observed. This is not an invariant check.';
      outcome = rolledBack === 0 && committed !== null ? 'success' : rolledBack > 0 ? 'error' : 'unknown';
      if (rolledBack > 0) message += ' Some requests rolled back; review the outcome counters and reason.';
    }
    else if (finished && difference !== null) { message = difference > 0 ? 'Completed — lost updates observed (balance below expected).' : 'Completed — balance exceeds expected.'; outcome = 'error'; }
    else if (finished) message = 'Workload finished, but complete balance results are unavailable. Match is not verified.';
    else if (supplied && typeof result.finished !== 'boolean') message = 'Demo completion status is unavailable; comparison is not verified.';
    else if (data.running === false && !finished) message = 'Waiting for the backend’s finished demo result; comparison is not verified.';
    const node = panel(`${action.kind === 'DEADLOCK' ? 'Opposing transfers' : action.kind === 'RACE' ? 'Unsafe deposit' : 'Safe deposit'} demo`, message, outcome);
    const entries = [['Committed', formatNumber(committed)], ['Rolled back', formatNumber(rolledBack)]];
    if (action.kind !== 'DEADLOCK') {
      entries.unshift(['Expected balance (minor)', formatNumber(expected)], ['Actual balance (minor)', formatNumber(actual)]);
      entries.push(['Lost / shortfall (minor)', finished && !action.interrupted && !inconsistent && difference !== null && difference >= 0 ? formatNumber(difference) : 'Not verified']);
    }
    details(node, entries);
    if (typeof result.reason === 'string' && result.reason) node.append(element('p', `Reason: ${result.reason}`));
    // Freeze completed evidence so later manual writes cannot change a demo result.
    action.finished = finished && (action.kind === 'DEADLOCK' || difference !== null);
  }

  function validateStats(data) {
    if (!isRecord(data) || typeof data.running !== 'boolean') throw new Error('Invalid stats response: expected an object with a boolean running field');
    return data;
  }

  function renderStats(data, freshAction = null) {
    validateStats(data);
    const running = data.running === true;
    const unsafe = data.mode === 'unsafe';
    const knownMode = unsafe || data.mode === 'safe';
    const databaseError = typeof data.database_error === 'string' ? data.database_error : '';
    $('database-error').textContent = databaseError ? `Database unavailable: ${databaseError}` : '';
    $('database-error').classList.toggle('hidden', !databaseError);
    $('system-status').textContent = running ? (unsafe ? 'Running / Unsafe' : 'Running') : 'Idle';
    $('state-dot').className = `state-dot ${running ? (unsafe ? 'unsafe' : 'running') : 'idle'}`;
    $('mode-badge').textContent = knownMode ? (unsafe ? 'UNSAFE' : 'SAFE') : 'MODE UNKNOWN';
    $('mode-badge').className = `mode-badge ${knownMode ? (unsafe ? 'unsafe' : 'safe') : 'unknown'}`;
    $('uptime').textContent = `${fixed(data.uptime_s, 1)}s`;
    $('throughput').textContent = fixed(data.throughput_tx_s, 1);
    $('avg-latency').textContent = fixed(data.avg_latency_ms, 2);
    $('p95-latency').textContent = fixed(data.p95_latency_ms, 2);
    $('total-balance').textContent = formatNumber(integer(data.total_balance_minor));
    [['committed', 'committed'], ['rolled-back', 'rolled_back'], ['retries', 'retries'], ['queue-depth', 'queue_depth'], ['active-workers', 'active_workers'], ['pool-leases', 'pool_leases_in_use'], ['lock-waits', 'lock_waits']].forEach(([id, key]) => { $(id).textContent = formatNumber(count(data[key])); });
    renderCheckBadge(data.latest_check);
    state.labels.push(new Date().toLocaleTimeString());
    state.throughput.push(number(data.throughput_tx_s) !== null && data.throughput_tx_s >= 0 ? data.throughput_tx_s : null);
    state.queue.push(count(data.queue_depth));
    state.active.push(count(data.active_workers));
    if (state.labels.length > 30) [state.labels, state.throughput, state.queue, state.active].forEach((values) => values.shift());
    const reasons = record(data.rollback_reasons);
    const reasonSummary = reasonLabels.map((key) => `${key}: ${formatNumber(count(reasons[key]))}`).join(' · ');
    $('reason-chart').setAttribute('aria-label', `Rollback reasons. ${reasonSummary}`);
    $('reason-summary').textContent = reasonLabels.filter((key) => count(reasons[key]) > 0).map((key) => `${key}: ${formatNumber(reasons[key])}`).join(' · ') || 'No positive counters reported';
    updateChart('throughput', state.labels, [state.throughput]);
    updateChart('outcomes', null, [[count(data.committed), count(data.rolled_back), count(data.retries)]]);
    updateChart('reasons', null, [reasonLabels.map((key) => count(reasons[key]))]);
    updateChart('queue', state.labels, [state.queue, state.active]);
    renderDemo(data, freshAction);
  }

  function emptyTable(body, columns, message) {
    const row = element('tr');
    const cell = element('td', message);
    cell.colSpan = columns;
    row.append(cell);
    body.replaceChildren(row);
  }

  function statusClass(value) {
    return value === 'COMMITTED' || value === 'ACTIVE' ? 'status-committed' : value === 'ROLLED_BACK' || value === 'CLOSED' || value === 'FROZEN' ? 'status-rolled' : 'status-pending';
  }

  function updatePageControls() {
    const previous = $('accounts-prev');
    const next = $('accounts-next');
    const focused = document.activeElement;
    previous.disabled = state.page === 1;
    next.disabled = !state.hasNext;
    if ((focused === previous || focused === next) && focused.disabled) $('accounts-wrap').focus({ preventScroll: true });
  }

  function renderAccounts(data) {
    const body = $('accounts-body');
    const fragment = document.createDocumentFragment();
    const previous = new Map();
    data.accounts.filter(isRecord).slice(0, state.limit).forEach((account) => {
      const row = element('tr');
      const id = count(account.id);
      const balance = integer(account.balance_minor);
      if (id !== null) {
        if (balance !== null && state.previousAccounts.has(id) && state.previousAccounts.get(id) !== balance) row.className = 'changed';
        previous.set(id, balance);
      }
      [id, account.owner, account.balance, account.status].forEach((value, index) => row.append(element('td', text(value), index === 3 ? statusClass(value) : '')));
      fragment.append(row);
    });
    state.previousAccounts = previous;
    body.replaceChildren(fragment);
    if (!body.childElementCount) emptyTable(body, 4, 'No accounts on this page.');
    state.hasNext = data.accounts.length === state.limit;
    $('accounts-page').textContent = `Page ${state.page} · ${state.limit} per page`;
    updatePageControls();
    $('accounts-status').textContent = `${data.accounts.filter(isRecord).length} account(s) shown. ${state.hasNext ? 'Next page available; total count is not supplied.' : 'End of accounts.'}`;
    $('accounts-status').classList.remove('status-rolled');
  }

  async function refreshAccounts(force = false) {
    if (state.stopped || (state.accountsLoading && !force)) return;
    const request = ++state.accountsRequest;
    const page = state.page;
    const limit = state.limit;
    state.accountsLoading = true;
    $('accounts-wrap').setAttribute('aria-busy', 'true');
    try {
      const data = await api(`/api/accounts?page=${page}&limit=${limit}`);
      if (state.stopped || request !== state.accountsRequest) return;
      if (!Array.isArray(data.accounts) || data.page !== page || data.limit !== limit) throw new Error('Invalid accounts response or pagination metadata');
      renderAccounts(data);
    } catch (error) {
      if (!state.stopped && request === state.accountsRequest) {
        $('accounts-status').textContent = `Accounts unavailable: ${error.message}. Displayed rows may be stale.`;
        $('accounts-status').classList.add('status-rolled');
      }
    } finally {
      if (request === state.accountsRequest) {
        state.accountsLoading = false;
        $('accounts-wrap').setAttribute('aria-busy', 'false');
      }
    }
  }

  function renderTransactions(data) {
    const rows = data.transactions.filter(isRecord).slice(0, 50).map((tx) => [
      text(tx.id), text(tx.type), `${count(tx.from) > 0 ? text(tx.from) : '—'} → ${count(tx.to) > 0 ? text(tx.to) : '—'}`,
      text(tx.amount), text(tx.status), text(tx.log_status)
    ]);
    const signature = JSON.stringify(rows);
    if (signature !== state.transactionSignature) {
      const wrap = $('transactions-wrap');
      const scrollTop = wrap.scrollTop;
      const fragment = document.createDocumentFragment();
      rows.forEach((values) => {
        const row = element('tr');
        values.forEach((value, index) => row.append(element('td', value, index >= 4 ? statusClass(value) : '')));
        fragment.append(row);
      });
      $('transactions-body').replaceChildren(fragment);
      if (!rows.length) emptyTable($('transactions-body'), 6, 'No transactions match this filter.');
      // The backend returns newest first, so following the latest means the top.
      wrap.scrollTop = $('transaction-autoscroll').checked ? 0 : scrollTop;
      state.transactionSignature = signature;
    }
    $('transactions-status').textContent = `${rows.length} latest transaction(s), newest first.`;
    $('transactions-status').classList.remove('status-rolled');
  }

  async function refreshTransactions(force = false) {
    if (state.stopped || (state.transactionsLoading && !force)) return;
    const request = ++state.transactionsRequest;
    const filter = $('transaction-filter').value;
    state.transactionsLoading = true;
    $('transactions-wrap').setAttribute('aria-busy', 'true');
    try {
      const suffix = filter ? `&status=${encodeURIComponent(filter)}` : '';
      const data = await api(`/api/transactions?limit=50${suffix}`);
      if (state.stopped || request !== state.transactionsRequest) return;
      if (!Array.isArray(data.transactions)) throw new Error('Invalid transactions response');
      renderTransactions(data);
    } catch (error) {
      if (!state.stopped && request === state.transactionsRequest) {
        $('transactions-status').textContent = `Transactions unavailable: ${error.message}. Displayed rows may be stale.`;
        $('transactions-status').classList.add('status-rolled');
      }
    } finally {
      if (request === state.transactionsRequest) {
        state.transactionsLoading = false;
        $('transactions-wrap').setAttribute('aria-busy', 'false');
      }
    }
  }

  function changePage(page, limit = state.limit) {
    state.page = page;
    state.limit = limit;
    state.hasNext = false;
    state.previousAccounts.clear();
    $('accounts-page').textContent = `Page ${page} · ${limit} per page`;
    updatePageControls();
    $('accounts-status').textContent = 'Loading accounts…';
    emptyTable($('accounts-body'), 4, 'Loading…');
    refreshAccounts(true);
  }

  const feed = { stream: null, reconnect: null, watchdog: null, poll: null, polling: false, generation: 0, failures: 0, received: 0, streaming: false };

  function feedStatus(mode, message) {
    $('feed-status').textContent = mode;
    $('feed-status').className = `live-pill${mode === 'LIVE' ? '' : ' unknown'}`;
    $('disconnected').textContent = message;
    $('disconnected').classList.toggle('hidden', mode === 'LIVE');
  }

  function stopPolling() {
    feed.polling = false;
    feed.generation += 1;
    window.clearTimeout(feed.poll);
    feed.poll = null;
  }

  function startPolling() {
    if (feed.polling || state.stopped) return;
    feed.polling = true;
    const generation = ++feed.generation;
    const poll = async () => {
      const received = feed.received;
      try {
        const data = await api('/api/stats');
        if (state.stopped || !feed.polling || generation !== feed.generation) return;
        if (received !== feed.received) return; // A newer SSE snapshot won this request race.
        renderStats(data);
        if (!feed.streaming) feedStatus('POLLING', typeof window.EventSource === 'function' ? 'Live stream unavailable — updating via polling; retrying stream automatically.' : 'Live stream unsupported — updating via polling.');
      } catch (error) {
        if (!state.stopped && feed.polling && generation === feed.generation && !feed.streaming) feedStatus('STALE', `Dashboard updates unavailable — ${error.message}. Retrying; displayed values may be stale.`);
      } finally {
        if (!state.stopped && feed.polling && generation === feed.generation) feed.poll = window.setTimeout(poll, 1800);
      }
    };
    poll();
  }

  function connectStream() {
    if (state.stopped || feed.stream || typeof window.EventSource !== 'function') return;
    window.clearTimeout(feed.reconnect);
    feed.reconnect = null;
    let stream;
    let validMessages = 0;
    const fail = () => {
      if (state.stopped || (stream && feed.stream !== stream)) return;
      window.clearTimeout(feed.watchdog);
      if (stream) stream.close();
      feed.stream = null;
      feed.streaming = false;
      feed.failures = Math.min(feed.failures + 1, 6);
      feedStatus('RECONNECTING', 'Live stream interrupted — trying polling and reconnecting. Displayed values may be stale.');
      startPolling();
      const delay = Math.min(30000, 1000 * (2 ** (feed.failures - 1))) + Math.floor(Math.random() * 250);
      window.clearTimeout(feed.reconnect);
      feed.reconnect = window.setTimeout(connectStream, delay);
    };
    const watch = () => {
      window.clearTimeout(feed.watchdog);
      feed.watchdog = window.setTimeout(fail, 10000);
    };
    try {
      stream = new window.EventSource('/api/stream');
      feed.stream = stream;
      stream.onmessage = (event) => {
        if (state.stopped || feed.stream !== stream) return;
        try {
          const data = validateStats(JSON.parse(event.data));
          renderStats(data);
          feed.received += 1;
          feed.streaming = true;
          feedStatus('LIVE', '');
          validMessages += 1;
          if (validMessages >= 3) { feed.failures = 0; stopPolling(); }
          watch();
        } catch (_) { fail(); }
      };
      stream.onerror = fail;
      // Includes an open connection that never sends a valid snapshot.
      watch();
    } catch (_) { fail(); }
  }

  async function refreshDemoStats(action) {
    try {
      const data = validateStats(await api('/api/stats'));
      if (!state.stopped && state.action === action) renderDemo(data, action);
    } catch (_) {
      // Live/polled updates can still observe the acknowledged run. Do not
      // convert an unavailable result into a successful completion.
      if (state.action === action && !action.ready) panel(action.kind, 'Demo accepted; waiting for a fresh stats snapshot. Completion is not verified.');
    }
  }

  function simulationPayload() {
    return {
      accounts: Number($('accounts').value), clients: Number($('clients').value), ops: Number($('ops').value),
      workers: Number($('workers').value), seed: Number($('seed').value), unsafe: $('unsafe').checked,
      isolation: $('isolation').value, hotspot_ratio: Number($('hotspot').value) / 100, scenario: 'MIXED'
    };
  }

  $('hotspot').addEventListener('input', () => {
    $('hotspot-value').value = `${$('hotspot').value}%`;
    $('hotspot').setAttribute('aria-valuetext', `${$('hotspot').value} percent`);
  });
  $('simulation-form').addEventListener('submit', async (event) => {
    event.preventDefault();
    const action = beginAction('SIMULATION', 'Starting simulation…');
    try {
      const result = await post('/api/simulate', simulationPayload());
      if (result.started !== true) throw new Error('Simulation start was not confirmed by the backend');
      if (state.action === action) panel('Simulation', 'Simulation started. Live counters show its progress.');
    } catch (error) { if (state.action === action) panel('Simulation', error.message, 'error'); }
  });
  $('stop-simulation').addEventListener('click', async () => {
    const action = state.action;
    if (action && !action.finished) action.interrupted = true;
    try {
      const result = await post('/api/simulate/stop');
      if (result.stopped !== true) throw new Error('Stop was not confirmed by the backend');
      showToast('Simulation stopped. Partial work is not a completed demo.');
      if (action && state.action === action && !action.finished) panel('Simulation stopped', 'The workload was interrupted; demo completion is not verified.');
    } catch (error) { showToast(error.message, true); }
  });
  $('tx-type').addEventListener('change', () => {
    const transfer = $('tx-type').value === 'TRANSFER';
    $('tx-to').disabled = !transfer;
    $('tx-to').required = transfer;
  });
  $('transaction-form').addEventListener('submit', async (event) => {
    event.preventDefault();
    state.action = null;
    setResult('transaction-result', 'Submitting transaction…');
    $('transaction-result').focus({ preventScroll: true });
    try {
      const result = await post('/api/tx', { type: $('tx-type').value, from: Number($('tx-from').value), to: Number($('tx-to').value), amount: $('tx-amount').value });
      const known = result.status === 'COMMITTED' || result.status === 'ROLLED_BACK';
      setResult('transaction-result', `${text(result.status, 'UNKNOWN')}: ${text(result.reason, known ? 'completed' : 'result not verified')} (attempts ${formatNumber(count(result.attempts))})`, result.status === 'COMMITTED' ? 'success' : known ? 'error' : 'unknown');
      refreshAccounts(); refreshTransactions();
    } catch (error) { setResult('transaction-result', error.message, 'error'); }
  });
  $('check-button').addEventListener('click', async () => {
    const action = beginAction('CHECK', 'Checking I1–I5…');
    try {
      const result = await post('/api/check');
      if (state.action !== action) return;
      const node = panel('Invariant results', 'Database consistency checklist');
      const outcome = appendCheck(node, result);
      node.classList.add(outcome);
      renderCheckBadge(result);
    } catch (error) { if (state.action === action) panel('Consistency check unavailable', error.message, 'error'); }
  });
  $('demo-check').addEventListener('click', () => $('check-button').click());
  $('recover-button').addEventListener('click', async () => {
    const action = beginAction('RECOVER', 'Resolving PENDING entries…');
    try {
      const result = await post('/api/recover');
      if (state.action === action) renderRecovery(result);
    } catch (error) {
      if (state.action !== action) return;
      if (isRecord(error.body) && (typeof error.body.succeeded === 'boolean' || isRecord(error.body.recovery))) renderRecovery(error.body, false, error);
      else panel('Recovery unavailable', error.message, 'error');
    }
  });
  $('reset-button').addEventListener('click', async () => {
    if (!window.confirm('Reset and reseed the development bank database?')) return;
    const action = beginAction('RESET', 'Resetting and reseeding…');
    try {
      const result = await post('/api/reset', { confirm: true });
      if (result.reset !== true) throw new Error('Reset was not confirmed by the backend');
      if (state.action === action) panel('Database reset', 'Database reset and reseeded. Run a fresh consistency check.');
      renderCheckBadge(null);
      changePage(1);
      refreshTransactions(true);
    } catch (error) { if (state.action === action) panel('Reset unavailable', error.message, 'error'); }
  });
  document.querySelectorAll('[data-demo]').forEach((button) => button.addEventListener('click', async () => {
    const name = button.dataset.demo;
    const action = beginAction(name, name === 'CRASH_RECOVER' ? 'Injecting an abort after debit, then rolling back, recovering and checking…' : 'Starting demo…');
    try {
      const result = await post('/api/demo', { name });
      if (state.action !== action) return;
      if (name === 'CRASH_RECOVER') renderRecovery(result, true);
      else {
        if (result.started !== true) throw new Error('Demo start was not confirmed by the backend');
        action.accepted = true;
        panel(`${name} demo accepted`, 'Waiting for this run’s stats. A 202 response does not mean the demo has completed.');
        refreshDemoStats(action);
      }
      refreshAccounts(); refreshTransactions();
    } catch (error) {
      if (state.action !== action) return;
      if (name === 'CRASH_RECOVER' && isRecord(error.body) && (typeof error.body.succeeded === 'boolean' || isRecord(error.body.recovery))) renderRecovery(error.body, true, error);
      else panel(`${name} demo unavailable`, error.message, 'error');
    }
  }));
  $('accounts-prev').addEventListener('click', () => { if (state.page > 1) changePage(state.page - 1); });
  $('accounts-next').addEventListener('click', () => { if (state.hasNext) changePage(state.page + 1); });
  $('accounts-limit').addEventListener('change', () => {
    const limit = Number($('accounts-limit').value);
    if ([10, 25, 50, 100].includes(limit)) changePage(1, limit);
  });
  $('transaction-filter').addEventListener('change', () => {
    state.transactionSignature = '';
    emptyTable($('transactions-body'), 6, 'Loading…');
    refreshTransactions(true);
  });
  $('transaction-autoscroll').addEventListener('change', () => { if ($('transaction-autoscroll').checked) $('transactions-wrap').scrollTop = 0; });

  function startUpdates() {
    state.stopped = false;
    startPolling();
    connectStream();
    refreshAccounts(); refreshTransactions();
    state.tableTimer = window.setInterval(() => { refreshAccounts(); refreshTransactions(); }, 1800);
  }

  window.addEventListener('pagehide', () => {
    state.stopped = true;
    state.accountsRequest += 1;
    state.transactionsRequest += 1;
    state.accountsLoading = false;
    state.transactionsLoading = false;
    if (feed.stream) feed.stream.close();
    feed.stream = null;
    feed.streaming = false;
    stopPolling();
    window.clearTimeout(feed.reconnect);
    window.clearTimeout(feed.watchdog);
    window.clearInterval(state.tableTimer);
  });
  window.addEventListener('pageshow', (event) => { if (event.persisted && state.stopped) startUpdates(); });
  drawCharts();
  startUpdates();
})();
