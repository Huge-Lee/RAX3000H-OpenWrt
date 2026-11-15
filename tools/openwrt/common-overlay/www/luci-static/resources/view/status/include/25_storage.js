'use strict';
'require baseclass';
'require rpc';

var callMountPoints = rpc.declare({
	object: 'luci',
	method: 'getMountPoints',
	expect: { result: [] }
});

function usage(entry) {
	if (!entry || !(entry.size > 0))
		return '?';

	var used = entry.size - entry.free,
	    percent = Math.max(0, Math.min(100, used / entry.size * 100));

	return E('div', {
		'class': 'cbi-progressbar',
		'title': '%s / %s (%d%%)'.format(
			String.format('%1024.2mB', used),
			String.format('%1024.2mB', entry.size),
			Math.floor(percent))
	}, E('div', { 'style': 'width:%.2f%%'.format(percent) }));
}

// Only these two filesystems represent usable storage on this device.
// Configuration bind mounts share /plugins and must not be counted again.
return baseclass.extend({
	title: _('Storage'),

	load: function() {
		return L.resolveDefault(callMountPoints(), []);
	},

	render: function(mounts) {
		var table = E('table', { 'class': 'table' });

		[
			[ '/plugins', '数据空间' ],
			[ '/tmp', _('Temp space') ]
		].forEach(function(field) {
			var entry = mounts.find(function(mount) {
				return mount.mount == field[0];
			});

			table.appendChild(E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td left', 'width': '33%' }, field[1]),
				E('td', { 'class': 'td left' }, [ usage(entry) ])
			]));
		});

		return table;
	}
});
