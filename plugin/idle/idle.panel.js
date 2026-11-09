"use strict";

// ---------------------------------------------------------------------------
// 自动关机面板
//
// 与 idle.so 同目录，由页面壳在启动时按插件清单加载。这一节只有一项
// plugins.idle.off_after_minutes；壳把当前层的那一节推过来，面板不取数据，
// 也不判断自己处在全局还是某只手柄之下。
// 留空 = 删掉这个键，插件用内置默认；
// 0 = 插件不武装（从不自动关机）；1..60 = 静置多少分钟后请手柄关机。
// ---------------------------------------------------------------------------

(() => {
  const { esc, desc, showToast } = DS5;
  const escAttr = esc;

  const KEY = 'off_after_minutes';
  const DEFAULT_MINUTES = 30;
  const MAX_MINUTES = 60;

  const view = {
    host: null,
    data: null,
    original: null,
    draft: null,
    error: '',
  };

  function valueText(){
    if (view.draft !== null) return view.draft;
    return view.original == null ? '' : view.original;
  }

  /// 手柄层没写这个键时，它从 global 继承到的值；global 也没写就是 null。
  function inheritedMinutes(){
    if (!view.data || !Array.isArray(view.data.inherited)) return null;
    for (const [key, value] of view.data.inherited) if (key === KEY) return value;
    return null;
  }

  /// 「删掉这个键之后回到哪里」的说明：上层继承到的值，或内置默认。
  function fallbackText(){
    const inherited = inheritedMinutes();
    if (inherited == null) return '内置默认 ' + DEFAULT_MINUTES + ' 分钟';
    if (inherited === '0') return '上层给的 0（不自动关机）';
    return '上层给的 ' + inherited + ' 分钟';
  }

  function otherRows(){
    if (!view.data || !Array.isArray(view.data.flat)) return '';
    const rows = view.data.flat.filter(([key]) => key !== KEY);
    if (!rows.length) return '';
    return `<div class="subdiv-light">页面不认识的键（原样保留）</div>` +
      rows.map(([key, value]) =>
        `<div class="kvrow"><code>${esc(key)}</code><span>${esc(value)}</span></div>`).join('');
  }

  function render(){
    if (!view.host) return;
    let body = '';
    if (view.error) body += `<div class="banner">${esc(view.error)}</div>`;
    if (!view.data){
      if (!view.error) body += `<p class="info-text">正在读取…</p>`;
    } else if (!view.data.editable){
      body += `<p class="info-text">这一层不能按行编辑：${esc(view.data.reason || '')}</p>
               <pre class="yaml">${esc(view.data.block || '（空）')}</pre>`;
    } else {
      const value = valueText();
      const inherited = inheritedMinutes();
      const state = view.original == null
        ? '这一层没有写这个键，生效的是' + fallbackText() + '。'
        : (view.original === '0' ? '写的是 0：不自动关机。' : '写的是 ' + view.original + ' 分钟。');
      body += `
        <div class="formgrid">
          <div class="field">
            <label for="idle-minutes">无操作多少分钟后关机${desc(
              '0 = 不自动关机（插件完全不武装）；留空 = 删掉这个键，回到' +
              fallbackText() + '；范围 0..' + MAX_MINUTES + '。')}</label>
            <input type="number" id="idle-minutes" data-role="minutes" min="0" max="${MAX_MINUTES}"
                   step="1" placeholder="${escAttr(inherited == null ? DEFAULT_MINUTES : inherited)}"
                   value="${escAttr(value)}" spellcheck="false">
          </div>
        </div>
        <p class="info-text" style="margin:8px 0 0">${esc(state)}</p>
        <p class="info-text" style="margin:8px 0 0">静止 = 摇杆居中、扳机为 0、按键与十字键都没动；
          到期后请手柄自己关机。</p>
        ${otherRows()}`;
    }
    view.host.innerHTML = `<section class="config-section">
      <div class="config-section-heading">
        <span class="config-section-icon">${esc('⏻')}</span>
        <div><h3>自动关机</h3></div>
      </div>
      ${body}
    </section>`;
  }

  /// 壳推过来的这一节；面板不自己取。
  function applyCtx(ctx){
    const section = (ctx && ctx.section) || null;
    view.data = section && !section.error ? section : null;
    view.error = section && section.error ? section.error : '';
    let found = null;
    if (view.data && Array.isArray(view.data.flat)){
      for (const [key, value] of view.data.flat) if (key === KEY) found = value;
    }
    view.original = found;
    view.draft = null;
    render();
  }

  function dirty(){ return collectEdits().length > 0; }

  function collectEdits(){
    if (!view.data || !view.data.editable) return [];
    const value = String(valueText()).trim();
    const before = view.original == null ? '' : String(view.original).trim();
    if (value === before) return [];
    if (value === ''){
      return view.original == null ? [] : [{ kind: 'section_remove', keys: [KEY] }];
    }
    return [{ kind: 'section_set', keys: [KEY], value }];
  }

  function bind(){
    const host = view.host;
    host.addEventListener('input', event => {
      const box = event.target.closest('[data-role="minutes"]');
      if (!box) return;
      view.draft = box.value;
      DS5.changed();
    });
  }

  function mount(host, ctx){
    view.host = host;
    applyCtx(ctx);
    bind();
  }

  async function refresh(ctx, force){
    if (!view.host || (!force && dirty())) return;
    applyCtx(ctx);
  }

  DS5.define({
    id: 'idle',
    title: '自动关机',
    mount,
    refresh,
    dirty,
    collect(){ return collectEdits(); },
  });
})();