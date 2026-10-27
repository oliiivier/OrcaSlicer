// Bridge with AssistantDialog (src/slic3r/AIAssistant/AssistantDialog.cpp).
// JS -> C++: window.wx.postMessage(JSON string)
// C++ -> JS: window.aiAssistant.onMessage(object)
(function () {
  'use strict';

  function send(msg) {
    if (window.wx && window.wx.postMessage) {
      window.wx.postMessage(JSON.stringify(msg));
    } else {
      console.warn('AI assistant: no host bridge, dropped', msg);
    }
  }

  const $ = (id) => document.getElementById(id);

  function addMessage(role, text) {
    const div = document.createElement('div');
    div.className = 'msg ' + role;
    div.textContent = text;
    $('messages').appendChild(div);
    div.scrollIntoView({ block: 'end' });
  }

  function applySettings(s) {
    $('provider').value = s.provider || 'anthropic';
    $('endpoint').value = s.endpoint || '';
    $('model').value = s.model || '';
    $('api_key').value = '';
    $('api_key').placeholder = s.has_api_key ? 'Stored (leave empty to keep)' : '';
    if (!s.has_api_key && s.provider === 'anthropic') {
      $('settings').hidden = false;
    }
  }

  const handlers = {
    settings: applySettings,
    chat_reply: (m) => addMessage('assistant', m.text),
    chat_error: (m) => addMessage('error', m.message),
  };

  window.aiAssistant = {
    onMessage(msg) {
      const handler = handlers[msg.cmd];
      if (handler) handler(msg);
      else console.warn('AI assistant: unknown message', msg);
    },
  };

  $('toggle-settings').addEventListener('click', () => {
    $('settings').hidden = !$('settings').hidden;
  });

  $('save-settings').addEventListener('click', () => {
    send({
      cmd: 'save_settings',
      provider: $('provider').value,
      endpoint: $('endpoint').value.trim(),
      model: $('model').value.trim(),
      api_key: $('api_key').value.trim(),
    });
    $('settings').hidden = true;
  });

  $('composer').addEventListener('submit', (e) => {
    e.preventDefault();
    const text = $('prompt').value.trim();
    if (!text) return;
    addMessage('user', text);
    $('prompt').value = '';
    send({ cmd: 'chat', text });
  });

  $('prompt').addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      // requestSubmit() is missing from older WebKitGTK builds.
      $('composer').dispatchEvent(new Event('submit', { cancelable: true }));
    }
  });

  send({ cmd: 'ready' });
})();
