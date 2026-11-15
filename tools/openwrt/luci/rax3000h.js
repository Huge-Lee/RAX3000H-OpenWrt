'use strict';
'require baseclass';
'require form';
'require uci';
'require rpc';

// Only the AP features implemented by the retained vendor driver belong here.
// The standard wireless view owns the list, actions and save/apply lifecycle.
const channels5 = [36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112,
	116, 120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165];

function validWidth(channel, mode) {
	if (/160$/.test(mode))
		return (channel >= 36 && channel <= 64) || (channel >= 100 && channel <= 128);
	return channel != 165 || !/40$|80$/.test(mode);
}

return baseclass.extend({
	status: rpc.declare({ object: 'luci-rpc', method: 'getWirelessDevices', expect: {} }),

	render: function(section, radioNet, renderStatus, toggle) {
		const radio = radioNet.getWifiDeviceName();
		const band = uci.get('wireless', radio, 'band');
		let o = section.option(form.SectionValue, '_device', form.NamedSection,
			radio, 'wifi-device', _('Device Configuration'));
		o.modalonly = true;
		let ss = o.subsection;
		ss.tab('general', _('General Setup'));

		o = ss.taboption('general', form.DummyValue, '_wifistat_modal', _('Status'));
		o.cfgvalue = () => renderStatus(null, radioNet);
		o.write = function() {};

		o = ss.taboption('general', form.DummyValue, '_error', _('Error'));
		o.load = id => this.status().then(data => {
			const state = data[id] || {};
			return state.retry_setup_failed ? _('Wireless startup failed. See System Log for details.') : null;
		});
		o.write = function() {};

		const disabled = radioNet.get('disabled') == '1' || uci.get('wireless', radio, 'disabled') == '1';
		o = ss.taboption('general', form.Button, '_toggle', disabled ? _('Wireless network is disabled') : _('Wireless network is enabled'));
		o.inputstyle = disabled ? 'apply' : 'reset';
		o.inputtitle = disabled ? _('Enable') : _('Disable');
		o.onclick = toggle;

		o = ss.taboption('general', form.DummyValue, '_band', _('Band'));
		o.cfgvalue = () => band == '2g' ? '2.4 GHz' : '5 GHz';
		o.write = function() {};

		const channels = band == '2g' ? Array.from({length: 13}, (_, i) => i + 1) : channels5;
		const channel = ss.taboption('general', form.ListValue.extend({
			renderWidget: function(id, index, value) {
				const automatic = value == 'auto';
				const active = Number(radioNet.getChannel());
				const fallback = Number(uci.get('wireless', id, 'rax_auto_fallback'));
				const displayed = automatic ? (channels.includes(active) ? active :
					channels.includes(fallback) ? fallback : channels[0]) : value;
				const widget = this.super('renderWidget', [id, index, String(displayed)]);
				const select = widget.querySelector('select');
				const readonly = this.readonly ?? this.map.readonly;
				const checkbox = E('input', {
					'type': 'checkbox', 'id': this.cbid(id) + '.auto',
					'change': () => {
						select.disabled = readonly || checkbox.checked;
						widget.dispatchEvent(new Event('change', { bubbles: true }));
					}
				});
				checkbox.checked = automatic;
				checkbox.disabled = !!readonly;
				select.disabled = readonly || automatic;
				return E('div', { 'style': 'display:flex;align-items:center;gap:0.75em;flex-wrap:wrap' }, [
					widget, E('label', { 'for': checkbox.id }, [ checkbox, ' 自动（unofficial）' ])
				]);
			},
			formvalue: function(id) {
				const checkbox = document.getElementById(this.cbid(id) + '.auto');
				return checkbox?.checked ? 'auto' : this.super('formvalue', [id]);
			},
			write: function(id, value) {
				if (value == 'auto' && uci.get('wireless', id, 'channel') != 'auto')
					uci.set('wireless', id, 'rax_auto_fallback', this.getUIElement(id).getValue());
				uci.set('wireless', id, 'channel', value);
			}
		}), 'channel', _('Channel'));
		channels.forEach(c => channel.value(String(c), String(c)));
		channel.rmempty = false;
		channel.description = '自动仅在该频段启动时选一次，运行中不换信道，保持所选带宽。框内显示当前实际信道，停用时显示后备信道。';

		const width = ss.taboption('general', form.ListValue, 'htmode', _('Mode') + ' / ' + _('Width'));
		const modes = ['NOHT', 'HT20', 'HT40', 'HE20', 'HE40'];
		if (band == '5g') modes.push('VHT20', 'VHT40', 'VHT80', 'VHT160', 'HE80', 'HE160');
		modes.forEach(mode => width.value(mode, mode == 'NOHT' ? _('Legacy') : mode));
		width.rmempty = false;
		width.validate = (id, value) => channel.formvalue(id) == 'auto'
			? true : validWidth(Number(channel.formvalue(id)), value)
				? true : _('This channel does not support the selected channel width.');

		o = ss.taboption('general', form.Value, 'rax_txpower', '发射功率');
		o.datatype = 'and(uinteger,range(0,300))';
		o.default = '300';
		o.rmempty = false;
		o.description = '范围 0–300，默认 300。数值越小发射功率越低，0 不代表关闭无线。';

		o = ss.taboption('general', form.ListValue, 'country', _('Country Code'));
		o.value('CN', 'CN');
		o.default = 'CN';
		o.rmempty = false;

		o = section.option(form.SectionValue, '_interface', form.NamedSection,
			radioNet.getName(), 'wifi-iface', _('Interface Configuration'));
		o.modalonly = true;
		ss = o.subsection;
		ss.tab('general', _('General Setup'));
		ss.tab('encryption', _('Wireless Security'));
		ss.tab('advanced', _('Advanced Settings'));

		o = ss.taboption('general', form.ListValue, 'mode', _('Mode'));
		o.value('ap', _('Access Point'));
		o.rmempty = false;

		o = ss.taboption('general', form.Value, 'ssid', _('SSID'));
		o.rmempty = false;
		o.validate = (id, value) => value && new TextEncoder().encode(value).length <= 32
			&& !/[\r\n]/.test(value) ? true : _('SSID must contain 1 to 32 bytes.');

		o = ss.taboption('general', form.ListValue, 'network', _('Network'));
		o.value('lan', 'lan');
		o.default = 'lan';
		o.rmempty = false;

		o = ss.taboption('general', form.Flag, 'hidden', _('Hide ESSID'));
		o.default = '0';

		const encryption = ss.taboption('encryption', form.ListValue, 'encryption', _('Encryption'));
		encryption.value('none', _('No Encryption'));
		encryption.value('psk2', 'WPA2-PSK (CCMP)');
		encryption.value('sae', 'WPA3-SAE');
		encryption.value('sae-mixed', 'WPA2-PSK/WPA3-SAE');
		encryption.rmempty = false;
		encryption.cfgvalue = id => (uci.get('wireless', id, 'encryption') || 'none').replace(/\+(ccmp|aes)$/, '');
		encryption.write = function(id, value) {
			uci.set('wireless', id, 'encryption', value);
			if (value == 'none') uci.unset('wireless', id, 'key');
		};

		o = ss.taboption('encryption', form.Value, 'key', _('Key'));
		o.password = true;
		o.rmempty = false;
		['psk2', 'sae', 'sae-mixed'].forEach(enc => o.depends('encryption', enc));
		o.validate = (id, value) => {
			const size = new TextEncoder().encode(value || '').length;
			return size >= 8 && size <= 63 && !/[\r\n]/.test(value)
				? true : _('Key must contain 8 to 63 bytes.');
		};
		// An inactive key field must not erase the value written by another option.
		o.remove = function() {};

		o = ss.taboption('encryption', form.ListValue, 'ieee80211w', _('802.11w Management Frame Protection'));
		o.value('0', _('Disabled')); o.value('1', _('Optional')); o.value('2', _('Required'));
		['psk2', 'sae', 'sae-mixed'].forEach(enc => o.depends('encryption', enc));
		o.cfgvalue = id => uci.get('wireless', id, 'ieee80211w') ??
			(encryption.cfgvalue(id) == 'sae' ? '2' : encryption.cfgvalue(id) == 'sae-mixed' ? '1' : '0');
		o.validate = (id, value) => {
			const enc = encryption.formvalue(id);
			return (enc == 'sae' && value != '2') || (enc == 'sae-mixed' && value == '0')
				? _('WPA3 requires Management Frame Protection.') : true;
		};

		o = ss.taboption('encryption', form.ListValue, 'sae_pwe', _('SAE PWE method'));
		o.value('0', _('Hunting and pecking only'));
		o.default = '0';
		o.description = _('This vendor hostapd requires hunting and pecking for compatible WPA3 connections.');
		o.depends('encryption', 'sae'); o.depends('encryption', 'sae-mixed');

		o = ss.taboption('advanced', form.Flag, 'isolate', _('Isolate Clients'));
		o.default = '0';
		o = ss.taboption('advanced', form.Value, 'maxassoc', _('Maximum number of associated clients'));
		o.datatype = 'range(1,128)';
		o.rmempty = true;
	}
});
