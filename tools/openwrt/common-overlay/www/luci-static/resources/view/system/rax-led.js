'use strict';
'require view';
'require form';

return view.extend({
    render: function() {
        var m = new form.Map('system', _('LED Configuration'));
        var s = m.section(form.NamedSection, 'led_control', 'led-control');
        s.addremove = false;
        var o = s.option(form.ListValue, 'mode', _('LED mode'),
            _('Default: green after startup. Off: turn off the status indicator.'));
        o.value('default', _('Default'));
        o.value('off', _('Off'));
        o.default = 'default';
        o.rmempty = false;
        return m.render();
    }
});
