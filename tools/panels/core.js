"use strict";

// ---------------------------------------------------------------------------
// 基础配置面板
//
// 随工具一起发，不来自插件目录。壳把当前层的基础视图推给它：选「全局」时
// 编辑用户配置的根级与 `global:`；选某只手柄时只编辑那一层。所有输入框共用
// 一条规则——留空就是删掉这个键，让下层（global 或内置默认）继续生效。
// 保存与运行状态都由壳统一处理，这里不读文件、不发请求。
// ---------------------------------------------------------------------------

(() => {
  const { esc, desc } = DS5;
  const escAttr = esc;

  /// 根级可编辑的三项；它们不属于任何一只手柄，只在全局层出现。
  const ROOT_FIELDS = [
    {
      key: 'enabled',
      label: '启用服务',
      type: 'select',
      hint: '留空等于内置默认（启用）。停用后服务停在 inactive，不重启。',
      options: [['', '留空（启用）'], ['true', '启用'], ['false', '停用']],
    },
    {
      key: 'bt_addr',
      label: '服务哪些手柄',
      type: 'text',
      placeholder: 'auto / list / XX:XX:XX:XX:XX:XX',
      hint: 'auto = 系统里发现的都服务；list = 只服务 controllers 列出的 MAC；写一个 MAC = 只服务那一只。',
    },
    {
      key: 'log_level',
      label: '日志级别',
      type: 'select',
      hint: '留空用内置级别。名字与 --help 里的 -v 取值同源。',
      options: [
        ['', '留空（内置默认）'],
        ['OFF', 'OFF'], ['FATAL', 'FATAL'], ['ERROR', 'ERROR'],
        ['WARNING', 'WARNING'], ['INFO', 'INFO'], ['DEBUG', 'DEBUG'],
        ['VERBOSE', 'VERBOSE'],
      ],
    },
  ];

  /// `global:` 与每个 `controllers.<MAC>` 共用的六项。
  const HANDHELD_FIELDS = [
    {
      key: 'bt_wait_s',
      label: '蓝牙等待（秒）',
      type: 'number',
      min: 0,
      step: 1,
      placeholder: '60',
      hint: 'worker 重试连接手柄的秒数；0 = 不等。',
    },
    {
      key: 'audio',
      label: '音频',
      type: 'select',
      hint: '关掉只桥接 HID，不建 PipeWire 的 sink 与 source。',
      options: [['', '留空（开）'], ['true', '开'], ['false', '关']],
    },
    {
      key: 'haptics_gain',
      label: '触觉增益',
      type: 'number',
      min: 0,
      max: 2,
      step: 0.1,
      placeholder: '1.0',
      hint: '0..2，1 = 原样，0 = 静音；只影响触觉，不影响喇叭。',
    },
    {
      key: 'speaker_gain',
      label: '扬声器增益',
      type: 'number',
      min: 0,
      max: 7,
      step: 1,
      placeholder: '2',
      hint: '0..7，写进每份 0x36 的 byte[37] 低 3 位，由手柄自己放大喇叭。',
    },
    {
      key: 'usb_report_interval_ms',
      label: 'USB 回报间隔',
      type: 'select',
      hint: '虚拟手柄的发送节拍：1/2/4 ms；0 = real-time，只在新样本到达时写一份。',
      options: [
        ['', '留空（1 ms）'], ['0', '0 = real-time'], ['1', '1 ms'],
        ['2', '2 ms'], ['4', '4 ms'],
      ],
    },
    {
      key: 'usb_model',
      label: 'USB 型号',
      type: 'select',
      hint: 'auto 跟随真手柄的 HID VID/PID；写死则强制按这个型号呈现。',
      options: [
        ['', '留空（auto）'], ['auto', 'auto（跟随手柄）'],
        ['dualsense', 'DualSense'], ['edge', 'DualSense Edge'],
      ],
    },
  ];

  const view = {
    host: null,
    /// 壳推过来的这一轮的 {@link base}、{@link layer} 与层条目 {@link entry}。
    ctx: null,
    original: new Map(),
    draft: new Map(),
  };

  function pathKey(path){ return JSON.stringify(path); }

  /// 当前层的手柄 MAC；全局层返回空串。
  function layerMac(){
    const layer = (view.ctx && view.ctx.layer) || '';
    return layer.startsWith('controllers.')
      ? layer.slice('controllers.'.length)
      : '';
  }

  function valueFor(path){
    const key = pathKey(path);
    if (view.draft.has(key)) return view.draft.get(key);
    const original = view.original.get(key);
    return original == null ? '' : original;
  }

  function readBase(){
    const base = (view.ctx && view.ctx.base) || {};
    view.original = new Map();
    for (const key of Object.keys(base.root || {})){
      const value = base.root[key];
      if (value != null) view.original.set(pathKey([key]), value);
    }
    for (const key of Object.keys(base.global || {})){
      const value = base.global[key];
      if (value != null) view.original.set(pathKey(['global', key]), value);
    }
    for (const controller of base.controllers || []){
      for (const key of Object.keys(controller.values || {})){
        const value = controller.values[key];
        if (value != null) view.original.set(pathKey(['controllers', controller.mac, key]), value);
      }
    }
  }

  function reset(){
    readBase();
    view.draft = new Map();
  }

  /// 这一只手柄从 global 继承到的值；没写的项返回 null。
  function inherited(key){
    const base = (view.ctx && view.ctx.base) || {};
    const value = (base.global || {})[key];
    return value == null ? null : String(value);
  }

  /// 一条「留空 = 删键」的输入；下拉框遇到文件里的非常规值时也把它列出来。
  function fieldHTML(path, field, placeholder){
    const value = valueFor(path);
    const id = 'core-' + path.join('-').replace(/[^A-Za-z0-9_-]/g, '-');
    let control = '';
    if (field.type === 'select'){
      let options = '';
      let known = false;
      for (const [optionValue, label] of field.options){
        if (optionValue === value) known = true;
        // 手柄层：空选项要说明"删掉这个键之后继承到的是 global 的哪个值"。
        const text = optionValue === '' && placeholder !== undefined
          ? '留空（继承 ' + placeholder + '）'
          : label;
        options += `<option value="${escAttr(optionValue)}"` +
                   `${optionValue === value ? ' selected' : ''}>${esc(text)}</option>`;
      }
      if (!known && value !== ''){
        options = `<option value="${escAttr(value)}" selected>文件里的值：${esc(value)}</option>` + options;
      }
      control = `<select id="${id}" data-path="${escAttr(pathKey(path))}">${options}</select>`;
    } else {
      const attrs = [
        `type="${field.type === 'number' ? 'number' : 'text'}"`,
        `id="${id}"`,
        `data-path="${escAttr(pathKey(path))}"`,
        `value="${escAttr(value)}"`,
        'spellcheck="false"',
      ];
      const hint = placeholder !== undefined ? placeholder : field.placeholder;
      if (hint) attrs.push(`placeholder="${escAttr(hint)}"`);
      if (field.min !== undefined) attrs.push(`min="${field.min}"`);
      if (field.max !== undefined) attrs.push(`max="${field.max}"`);
      if (field.step !== undefined) attrs.push(`step="${field.step}"`);
      control = `<input ${attrs.join(' ')}>`;
    }
    const label = `<label for="${id}">${esc(field.label)}${desc(field.hint)}</label>`;
    return `<div class="field">${label}${control}</div>`;
  }

  function fieldsHTML(prefix, fields, placeholders){
    return fields.map(field => fieldHTML(
      prefix.concat(field.key),
      field,
      placeholders ? placeholders[field.key] : undefined,
    )).join('');
  }

  function baseErrorHTML(){
    const base = (view.ctx && view.ctx.base) || {};
    return base.error ? `<div class="banner">${esc(base.error)}</div>` : '';
  }

  function globalHTML(){
    return `${baseErrorHTML()}
      <section class="config-section config-section-featured">
        <div class="config-section-heading">
          <span class="config-section-icon">${esc('≡')}</span>
          <div><h3>基础配置 · 全局</h3>
            <p>留空 = 删掉这个键，回到内置默认。</p></div>
        </div>
        <div class="subdiv">根级</div>
        <div class="formgrid">${fieldsHTML([], ROOT_FIELDS)}</div>
        <div class="subdiv">global（所有手柄的默认值）</div>
        <div class="formgrid">${fieldsHTML(['global'], HANDHELD_FIELDS)}</div>
      </section>`;
  }

  function controllerHTML(mac){
    const entry = (view.ctx && view.ctx.entry) ||
                  { mac, connected: false, configured: false, model: '' };
    const pills =
      `<span class="pill${entry.connected ? ' ok' : ''}">${entry.connected ? '已连接' : '未连接'}</span> ` +
      `<span class="pill${entry.configured ? ' ok' : ' warn'}">` +
      `${entry.configured ? '文件里已有' : '还没写进文件'}</span>` +
      (entry.model ? ` <span class="pill">${esc(entry.model)}</span>` : '');
    // 灰字是 global 里的值；global 没写时回到字段自己的内置默认。
    const placeholders = {};
    for (const field of HANDHELD_FIELDS){
      const value = inherited(field.key);
      placeholders[field.key] = value == null ? undefined : 'global: ' + value;
    }
    return `${baseErrorHTML()}
      <section class="config-section config-section-featured">
        <div class="config-section-heading">
          <span class="config-section-icon">${esc('◎')}</span>
          <div><h3>基础配置 · 手柄 <code>${esc(mac)}</code></h3>
            <p>${pills}</p></div>
        </div>
        <div class="formgrid">${fieldsHTML(['controllers', mac], HANDHELD_FIELDS, placeholders)}</div>
        <p class="info-text" style="margin:10px 0 0">留空 = 删掉这个键；灰字是继承到的值。</p>
      </section>`;
  }

  function render(){
    if (!view.host) return;
    view.host.innerHTML = layerMac() ? controllerHTML(layerMac()) : globalHTML();
  }

  /// 只把与文件里不同的项交出去；改回原值等于没改。
  function collectEdits(){
    const edits = [];
    const touched = new Set([...view.original.keys(), ...view.draft.keys()]);
    for (const key of touched){
      const path = JSON.parse(key);
      const original = view.original.has(key) ? view.original.get(key) : null;
      const raw = view.draft.has(key)
        ? view.draft.get(key)
        : (original == null ? '' : original);
      const value = String(raw).trim();
      const before = original == null ? '' : String(original).trim();
      if (value === before) continue;
      if (value === ''){
        if (original != null) edits.push({ kind: 'remove', path });
      } else {
        edits.push({ kind: 'scalar', path, value });
      }
    }
    // 新加的手柄由壳补出 `controllers.<MAC>:`；这里只发键值，路径本身带出条目。
    return edits;
  }

  function dirty(){ return collectEdits().length > 0; }

  function setDraft(path, value){
    view.draft.set(pathKey(path), value);
    DS5.changed();
  }

  function bind(){
    const host = view.host;
    const onEdit = event => {
      const element = event.target.closest('[data-path]');
      if (!element) return;
      setDraft(JSON.parse(element.dataset.path), element.value);
    };
    host.addEventListener('input', onEdit);
    host.addEventListener('change', onEdit);
  }

  function mount(host, ctx){
    view.host = host;
    view.ctx = ctx || null;
    reset();
    render();
    bind();
  }

  async function refresh(ctx, force){
    if (!view.host) return;
    if (!force && dirty()) return;
    view.ctx = ctx || null;
    reset();
    render();
  }

  DS5.define({
    id: 'core',
    title: '基础配置',
    mount,
    refresh,
    dirty,
    collect(){ return collectEdits(); },
  });
})();
