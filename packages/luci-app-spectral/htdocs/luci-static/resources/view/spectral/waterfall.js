'use strict';
'require view';
'require rpc';
'require poll';
'require ui';

var callStatus = rpc.declare({ object: 'spectral', method: 'status', expect: {} });
var callStart  = rpc.declare({ object: 'spectral', method: 'start', params: [ 'phy', 'bins', 'mode', 'preset' ], expect: {} });
var callStop   = rpc.declare({ object: 'spectral', method: 'stop', expect: {} });
var callGet    = rpc.declare({ object: 'spectral', method: 'get', params: [ 'cursor' ], expect: {} });

var MAX_ROWS = 300;	/* 30 s of history at 10 rows/s */
var POLL_S   = 1;	/* LuCI poll granularity is 1 s */

var PALETTES = {
	turbo: [
		[ 48, 18, 59], [ 64, 62, 133], [ 70,107, 227], [ 62,164, 227],
		[ 61,208,175], [104,239, 101], [199,233, 47], [252,166,  26],
		[242, 60,  5], [122,  4,  3]
	],
	inferno: [
		[  0,  0,  4], [ 40, 11, 84], [101, 21,110], [159, 42, 99],
		[212, 72, 66], [245,125, 21], [250,193, 39], [252,255,164]
	],
	viridis: [
		[ 68,  1, 84], [ 72, 40,120], [ 62, 74,137], [ 49,104,142],
		[ 38,130,142], [ 31,158,137], [ 53,183,121], [109,205, 89],
		[180,222, 44], [253,231, 37]
	],
	gray: [ [ 0, 0, 0], [255,255,255] ]
};

function make_lut(name) {
	var pts = PALETTES[name] || PALETTES.turbo;
	var lut = new Array(256);

	for (var i = 0; i < 256; i++) {
		var t = i / 255 * (pts.length - 1);
		var a = Math.floor(t), b = Math.min(a + 1, pts.length - 1), f = t - a;

		lut[i] = [
			Math.round(pts[a][0] * (1 - f) + pts[b][0] * f),
			Math.round(pts[a][1] * (1 - f) + pts[b][1] * f),
			Math.round(pts[a][2] * (1 - f) + pts[b][2] * f)
		];
	}
	return lut;
}

function freq_step(span) {
	var steps = [ 1, 2, 5, 10, 20, 50, 100 ];

	for (var i = 0; i < steps.length; i++)
		if (span / steps[i] <= 8)
			return steps[i];
	return 100;
}

return view.extend({
	/* ---------------------------------------------------------------- */
	load: function() {
		return callStatus();
	},

	render: function(status) {
		this.phy = 0;
		this.bins = 128;
		this.dbMin = -130;
		this.dbMax = -95;
		this.manualRange = false;
		this.paletteName = 'turbo';
		this.lut = make_lut(this.paletteName);

		this.cols = 0;
		this.freq = 0;
		this.width = 0;
		this.epoch = -1;
		this.cursor = 0;
		this.rows = [];
		this.maxHold = null;
		this.running = false;
		this.paused = false;
		this.mode = 'channel';
		this.buf = null;

		var self = this;

		this.statusEl = E('div', { 'class': 'spectral-status' }, _('Idle'));
		this.errorEl = E('div', { 'class': 'alert-message warning',
			'style': 'display:none; margin-top:8px' });

		this.spCanvas = E('canvas', {
			width: 1024, height: 150,
			'style': 'width:100%; display:block; background:#0b0d12; border:1px solid #333'
		});
		this.wfCanvas = E('canvas', {
			width: 1024, height: 400,
			'style': 'width:100%; display:block; background:#0b0d12; border:1px solid #333; cursor:crosshair'
		});
		this.spCtx = this.spCanvas.getContext('2d');
		this.wfCtx = this.wfCanvas.getContext('2d');

		this.off = document.createElement('canvas');
		this.off.width = 1;
		this.off.height = 1;
		this.offCtx = this.off.getContext('2d');

		this.tooltip = E('div', {
			'style': 'position:absolute; display:none; pointer-events:none; ' +
				 'background:rgba(0,0,0,.85); color:#eee; font-size:11px; ' +
				 'padding:2px 6px; border:1px solid #555; border-radius:3px; z-index:10'
		});
		this.wrap = E('div', { 'style': 'position:relative' }, [ this.spCanvas, this.wfCanvas, this.tooltip ]);

		this.wfCanvas.addEventListener('mousemove', function(ev) { self.handleHover(ev); });
		this.wfCanvas.addEventListener('mouseleave', function() {
			self.tooltip.style.display = 'none';
		});

		this.phySel = E('select', { 'class': 'cbi-input-select' }, [
			E('option', { value: 0 }, 'phy0 (2.4 GHz)'),
			E('option', { value: 1 }, 'phy1 (5 GHz)')
		]);
		this.binsSel = E('select', { 'class': 'cbi-input-select' }, [
			E('option', { value: 64 }, '64 bins'),
			E('option', { value: 128 }, '128 bins'),
			E('option', { value: 256 }, '256 bins')
		]);
		this.binsSel.value = '128';

		this.modeSel = E('select', { 'class': 'cbi-input-select' }, [
			E('option', { value: 'channel' }, _('Single channel')),
			E('option', { value: '2g' }, _('Sweep 2.4 GHz (ch 1-13)')),
			E('option', { value: '5g' }, _('Sweep 5 GHz (ch 36-165)')),
			E('option', { value: '5g-fast' }, _('Sweep 5 GHz (fast)'))
		]);
		this.modeSel.addEventListener('change', function() {
			self.applyMode();
		});

		this.paletteSel = E('select', { 'class': 'cbi-input-select' }, [
			E('option', { value: 'turbo' }, _('Turbo')),
			E('option', { value: 'inferno' }, _('Inferno')),
			E('option', { value: 'viridis' }, _('Viridis')),
			E('option', { value: 'gray' }, _('Grayscale'))
		]);
		this.paletteSel.addEventListener('change', function() {
			self.paletteName = self.paletteSel.value;
			self.lut = make_lut(self.paletteName);
			self.draw();
		});

		this.dbMinEl = E('input', { 'class': 'cbi-input-text', 'type': 'number', 'style': 'width:5em',
			'value': String(this.dbMin) });
		this.dbMaxEl = E('input', { 'class': 'cbi-input-text', 'type': 'number', 'style': 'width:5em',
			'value': String(this.dbMax) });
		var applyRange = function() {
			var lo = parseFloat(self.dbMinEl.value), hi = parseFloat(self.dbMaxEl.value);

			if (!isNaN(lo) && !isNaN(hi) && hi > lo) {
				self.manualRange = true;
				self.dbMin = lo;
				self.dbMax = hi;
				self.draw();
			}
		};
		this.dbMinEl.addEventListener('change', applyRange);
		this.dbMaxEl.addEventListener('change', applyRange);

		this.startBtn = E('button', {
			'type': 'button',
			'class': 'cbi-button cbi-button-apply',
			'click': ui.createHandlerFn(this, 'handleStart')
		}, _('Start'));
		this.stopBtn = E('button', {
			'type': 'button',
			'class': 'cbi-button cbi-button-reset',
			'click': ui.createHandlerFn(this, 'handleStop')
		}, _('Stop'));
		this.pauseBtn = E('button', {
			'type': 'button',
			'class': 'cbi-button',
			'click': ui.createHandlerFn(this, 'handlePause')
		}, _('Pause'));
		this.autoBtn = E('button', {
			'type': 'button',
			'class': 'cbi-button',
			'click': ui.createHandlerFn(this, 'handleAutoRange')
		}, _('Auto range'));

		poll.add(function() { return self.handlePoll(); }, POLL_S);

		this.updateStatus(status);
		this.draw();

		/* adopt a scan started by another session */
		if (status && status.running) {
			this.phy = status.phy || 0;
			this.phySel.value = String(this.phy);
			this.cursor = 0;
		}

		return E('div', { 'class': 'cbi-map' }, [
			E('h2', {}, _('Wi-Fi Spectrum')),
			E('div', { 'class': 'cbi-map-descr' },
				_('Live FFT waterfall from the ath10k spectral-scan engine: ' +
				  'a single channel, or a channel sweep stitched into a panorama.')),
			E('div', { 'class': 'cbi-section' }, [
				E('div', { 'style': 'display:flex; flex-wrap:wrap; gap:12px; align-items:center; margin-bottom:8px' }, [
					E('label', {}, [ _('Mode') + ': ', this.modeSel ]),
					E('label', {}, [ _('Radio') + ': ', this.phySel ]),
					E('label', {}, [ _('FFT bins') + ': ', this.binsSel ]),
					E('label', {}, [ _('Colour map') + ': ', this.paletteSel ]),
					this.startBtn, this.stopBtn, this.pauseBtn
				]),
				E('div', { 'style': 'display:flex; flex-wrap:wrap; gap:12px; align-items:center; margin-bottom:8px' }, [
					E('label', {}, [ _('dBm range') + ': ', this.dbMinEl, ' … ', this.dbMaxEl,
						' ', this.autoBtn ])
				]),
				this.statusEl,
				this.errorEl,
				E('div', { 'style': 'margin-top:8px' }, [ this.wrap ]),
				E('div', { 'class': 'cbi-note', 'style': 'margin-top:8px' },
					_('Requires the ath10k spectral_scan debugfs interface ' +
					  '(CONFIG_PACKAGE_ATH_SPECTRAL + CONFIG_RELAY). ' +
					  'Sweep mode briefly takes the radio off-channel; ' +
					  'clients may notice short interruptions. ' +
					  'The scan stops automatically 60 s after the last poll.'))
			])
		]);
	},

	/* ---------------------------------------------------------------- */
	updateStatus: function(st) {
		if (!st || typeof st !== 'object')
			return;

		this.running = !!st.running;
		if (st.epoch != null)
			this.epoch = st.epoch;
		if (st.cols)
			this.cols = st.cols;
		if (st.freq)
			this.freq = st.freq;
		if (st.width)
			this.width = st.width;

		if (st.error) {
			this.errorEl.style.display = '';
			this.errorEl.textContent = st.error;
		} else {
			this.errorEl.style.display = 'none';
		}

		if (!this.running) {
			this.statusEl.textContent = this.rows.length ?
				_('Stopped — showing the last capture') : _('Stopped');
			return;
		}

		if (st.mode === 'sweep' && st.sweep_n) {
			var stxt = 'sweep ' + st.sweep_n + ' channels' +
				' · ' + (st.sweep_cycles || 0) + ' cycles' +
				' · now ' + (st.sweep_freq || 0) + ' MHz' +
				' · ' + (st.rate || 0) + ' samples/s';

			if (this.rows.length)
				stxt += ' · ' + this.rows.length + ' rows';
			this.statusEl.textContent = stxt;
			return;
		}

		var txt = 'phy' + this.phy +
			' · ' + (this.freq || 0) + ' MHz' +
			' · ' + (this.width || 0) + ' MHz span' +
			' · ' + (this.cols || 0) + ' bins' +
			' · ' + (st.rate || 0) + ' samples/s';

		if (this.rows.length)
			txt += ' · ' + this.rows.length + ' rows (' +
				Math.round(this.rows.length * (st.row_ms || 100) / 1000) + ' s)';

		this.statusEl.textContent = txt;
	},

	applyMode: function() {
		var m = this.modeSel && this.modeSel.value;

		if (m === '2g') {
			this.phySel.value = '0';
			this.binsSel.value = '64';
		} else if (m === '5g' || m === '5g-fast') {
			this.phySel.value = '1';
			this.binsSel.value = '256';
		}
	},

	handleStart: function() {
		var self = this;
		var preset = (this.modeSel && this.modeSel.value) || 'channel';
		var mode = (preset === 'channel') ? 'channel' : 'sweep';

		this.errorEl.style.display = 'none';
		this.statusEl.textContent = _('Starting…');
		this.mode = mode;
		this.phy = parseInt(this.phySel.value, 10) || 0;
		this.bins = parseInt(this.binsSel.value, 10) || 128;

		return callStart(this.phy, this.bins, mode, mode === 'sweep' ? preset : '').then(function(res) {
			if (!res || !res.ok) {
				self.errorEl.style.display = '';
				self.errorEl.textContent = (res && res.error) || _('Failed to start spectral scan');
				return;
			}
			self.errorEl.style.display = 'none';
			self.running = true;
			self.epoch = -1;
			self.cursor = 0;
			self.cols = 0;
			self.rows = [];
			self.maxHold = null;
			self.draw();
			return self.handlePoll();
		}).catch(function(err) {
			self.errorEl.style.display = '';
			self.errorEl.textContent = _('ubus error: ') + err.message;
		});
	},

	handleStop: function() {
		var self = this;

		return callStop().then(function() {
			self.running = false;
			self.updateStatus({ running: 0, cols: self.cols });
			self.draw();
		}).catch(function(err) {
			self.errorEl.style.display = '';
			self.errorEl.textContent = _('ubus error: ') + err.message;
		});
	},

	autoRange: function() {
		if (!this.maxHold || !this.maxHold.length)
			return;

		var vals = Array.prototype.slice.call(this.maxHold);
		vals.sort(function(a, b) { return a - b; });

		var lo = vals[Math.floor(vals.length * 0.02)];
		var hi = vals[Math.min(vals.length - 1, Math.floor(vals.length * 0.995))];

		if (hi - lo < 10)
			hi = lo + 10;

		this.dbMin = Math.max(-140, Math.floor(lo) - 2);
		this.dbMax = Math.min(0, Math.ceil(hi) + 2);
		this.dbMinEl.value = String(this.dbMin);
		this.dbMaxEl.value = String(this.dbMax);
	},

	handleAutoRange: function() {
		this.manualRange = false;
		this.autoRange();
		this.draw();
	},

	handlePause: function() {
		this.paused = !this.paused;
		this.pauseBtn.textContent = this.paused ? _('Resume') : _('Pause');
	},

	handlePoll: function() {
		var self = this;

		if (this.paused || !this.wfCanvas)
			return;

		return callGet(this.cursor).then(function(res) {
			if (!res || !res.ok)
				return;

			self.updateStatus(res);

			if (res.reset || res.epoch !== self.epoch || res.cols !== self.cols) {
				self.epoch = res.epoch;
				self.cols = res.cols;
				self.freq = res.freq;
				self.width = res.width;
				self.rows = [];
				self.maxHold = null;
				self.cursor = 0;
			} else {
				if (res.freq)
					self.freq = res.freq;
				if (res.width)
					self.width = res.width;
			}

			if (res.rows > 0 && res.data && res.data.length)
				self.appendRows(res.data, res.cols);

			self.cursor = res.cursor || 0;
			if (!self.manualRange && self.maxHold)
				self.autoRange();
			self.draw();
		}).catch(function(err) {
			self.errorEl.style.display = '';
			self.errorEl.textContent = _('ubus error: ') + err.message;
		});
	},

	appendRows: function(data, cols) {
		if (!cols)
			return;

		if (!this.maxHold || this.maxHold.length !== cols) {
			this.maxHold = new Int16Array(cols).fill(-128);
			this.rows = [];
		}

		for (var off = 0; off + cols <= data.length; off += cols) {
			var row = new Int16Array(cols);

			for (var i = 0; i < cols; i++) {
				var db = data[off + i] - 128;

				row[i] = db;
				if (db > this.maxHold[i])
					this.maxHold[i] = db;
			}
			this.rows.push(row);
		}

		if (this.rows.length > MAX_ROWS)
			this.rows.splice(0, this.rows.length - MAX_ROWS);
	},

	/* ---------------------------------------------------------------- */
	draw: function() {
		this.drawSpectrum();
		this.drawWaterfall();
	},

	dbToIdx: function(db) {
		var t = (db - this.dbMin) / (this.dbMax - this.dbMin);

		if (t <= 0) return 0;
		if (t >= 1) return 255;
		return (t * 255) | 0;
	},

	drawWaterfall: function() {
		var ctx = this.wfCtx, W = this.wfCanvas.width, H = this.wfCanvas.height;

		ctx.clearRect(0, 0, W, H);
		if (!this.cols || !this.rows.length)
			return;

		if (this.off.width !== this.cols || this.off.height !== MAX_ROWS) {
			this.off.width = this.cols;
			this.off.height = MAX_ROWS;
		}

		if (!this.buf || this.buf.width !== this.cols || this.buf.height !== MAX_ROWS)
			this.buf = this.offCtx.createImageData(this.cols, MAX_ROWS);

		var p = this.buf.data, lut = this.lut, rows = this.rows, n = rows.length;

		for (var r = 0; r < MAX_ROWS; r++) {
			var ri = n - 1 - r, base = r * this.cols * 4;

			if (ri < 0) {
				for (var i = 0; i < this.cols; i++) {
					var o0 = base + i * 4;
					p[o0] = p[o0 + 1] = p[o0 + 2] = 8;
					p[o0 + 3] = 255;
				}
				continue;
			}

			var row = rows[ri];
			for (var b = 0; b < this.cols; b++) {
				var c = lut[this.dbToIdx(row[b])], o = base + b * 4;
				p[o] = c[0]; p[o + 1] = c[1]; p[o + 2] = c[2]; p[o + 3] = 255;
			}
		}

		this.offCtx.putImageData(this.buf, 0, 0);
		ctx.imageSmoothingEnabled = false;
		ctx.drawImage(this.off, 0, 0, this.cols, MAX_ROWS, 0, 0, W, H);
	},

	drawSpectrum: function() {
		var ctx = this.spCtx, W = this.spCanvas.width, H = this.spCanvas.height;
		var padL = 48, padR = 12, padT = 10, padB = 22;
		var pw = W - padL - padR, ph = H - padT - padB;

		ctx.clearRect(0, 0, W, H);
		ctx.font = '11px sans-serif';
		ctx.lineWidth = 1;

		if (!this.cols) {
			ctx.fillStyle = '#8a919e';
			ctx.textAlign = 'center';
			ctx.fillText(_('no samples yet — press Start'), W / 2, H / 2);
			return;
		}

		var self = this;
		var yOf = function(db) {
			var t = (db - self.dbMin) / (self.dbMax - self.dbMin);

			t = Math.max(0, Math.min(1, t));
			return padT + (1 - t) * ph;
		};
		var xOf = function(i) {
			return padL + (self.cols > 1 ? i / (self.cols - 1) : 0.5) * pw;
		};

		/* dB grid */
		ctx.strokeStyle = '#252a34';
		ctx.fillStyle = '#8a919e';
		ctx.textAlign = 'right';
		ctx.textBaseline = 'middle';
		var dbStep = Math.max(5, Math.round((this.dbMax - this.dbMin) / 6 / 5) * 5);

		for (var db = Math.ceil(this.dbMin / dbStep) * dbStep; db <= this.dbMax; db += dbStep) {
			var y = yOf(db);

			ctx.beginPath();
			ctx.moveTo(padL, y);
			ctx.lineTo(W - padR, y);
			ctx.stroke();
			ctx.fillText(db + ' dBm', padL - 5, y);
		}

		/* frequency ticks */
		var span = this.width || 0;
		var start = (this.freq || 0) - span / 2;
		var step = freq_step(span || 20);

		ctx.textAlign = 'center';
		ctx.textBaseline = 'top';

		for (var k = 0; k * step <= span + 0.001; k++) {
			var f = start + k * step;
			var x = padL + (k * step / (span || 1)) * pw;

			if (x < padL - 1 || x > W - padR + 1)
				continue;

			ctx.strokeStyle = '#1d222b';
			ctx.beginPath();
			ctx.moveTo(x, padT);
			ctx.lineTo(x, padT + ph);
			ctx.stroke();
			ctx.fillStyle = '#8a919e';
			ctx.fillText(Math.round(f) + '', x, padT + ph + 5);
		}

		/* max-hold */
		if (this.maxHold) {
			ctx.strokeStyle = 'rgba(90,160,255,.45)';
			ctx.lineWidth = 1;
			ctx.beginPath();
			for (var i = 0; i < this.cols; i++) {
				var yy = yOf(this.maxHold[i]);

				if (i == 0) ctx.moveTo(xOf(i), yy); else ctx.lineTo(xOf(i), yy);
			}
			ctx.stroke();
		}

		/* latest frame */
		if (this.rows.length) {
			var row = this.rows[this.rows.length - 1];

			ctx.strokeStyle = '#ffb454';
			ctx.lineWidth = 1.5;
			ctx.beginPath();
			for (var j = 0; j < this.cols; j++) {
				var yv = yOf(row[j]);

				if (j == 0) ctx.moveTo(xOf(j), yv); else ctx.lineTo(xOf(j), yv);
			}
			ctx.stroke();
		}

		ctx.fillStyle = '#8a919e';
		ctx.textAlign = 'left';
		ctx.textBaseline = 'top';
		ctx.fillText(span ? (span + ' MHz span · ' + this.freq + ' MHz centre') : '', padL, 0);
	},

	handleHover: function(ev) {
		if (!this.cols || !this.width)
			return;

		var rect = this.wfCanvas.getBoundingClientRect();
		var rel = (ev.clientX - rect.left) / rect.width;

		rel = Math.max(0, Math.min(1, rel));

		var i = Math.min(this.cols - 1, Math.round(rel * (this.cols - 1)));
		var span = this.width, f = (this.freq - span / 2) + (i + 0.5) * span / this.cols;
		var line = (this.rows.length ? this.rows[this.rows.length - 1][i] : null);
		var max = (this.maxHold ? this.maxHold[i] : null);
		var txt = f.toFixed(1) + ' MHz';

		if (line != null)
			txt += ' · ' + line + ' dBm';
		if (max != null)
			txt += ' (max ' + max + ')';

		this.tooltip.textContent = txt;
		this.tooltip.style.display = 'block';
		this.tooltip.style.left = (ev.clientX - this.wrap.getBoundingClientRect().left + 12) + 'px';
		this.tooltip.style.top  = (ev.clientY - this.wrap.getBoundingClientRect().top - 24) + 'px';
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
