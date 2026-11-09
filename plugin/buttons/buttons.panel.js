"use strict";

(() => {

// ---------------------------------------------------------------------------
// 按键映射面板
//
// 与 buttons.so 同目录，由页面壳在启动时按插件清单加载。壳按当前层取回
// plugins.buttons 这一节并连同词表一起推过来；本文件只把这一节的规则模型
// 画成工作台，改动收集回去由壳补上插件名与层再保存。面板不读文件、不发
// 请求，也不知道这一节属于全局还是某只手柄。
// ---------------------------------------------------------------------------

/// 壳提供的工具；面板不重复实现一份。
const { esc, desc, showToast, clone } = DS5;
/// 名称与壳同源：属性位置的转义与文本相同。
const escAttr = esc;

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------

/// 映射工作台里的四类规则；中间那一行就是它们。
const KINDS = [
  {id:'remap',    label:'基础按键'},
  {id:'turbo',    label:'连点'},
  {id:'shortcut', label:'组合键'},
  {id:'macro',    label:'宏'},
];

const TIPS = {
  remap: '一个源一条映射；“抹掉”等于 disable，“保留源键”两边都上报。同一帧内规则不串联。',
  turbo: '按住时按固定频率连点；实际频率不高于手柄上报率。',
  shortcut: '同时按住才算命中；单击/双击在松开时判定，长按到期触发。动作只有关机。',
  macro: '触发时同时按下目标键，约 80 毫秒后松开；按住更久也只发一次。',
  axis: '先反向，再定中心，写回同一帧。扳机反向后松开仍是 0。',
  other: '页面没识别的写法，原样写回；点表示嵌套。',
};

/// [7] 低 nibble 的十字键 hat → 方向位；0..7 顺时针从正北起，9..15 手柄不报。
const HAT_TOKENS = [
  ['dpad_up'],
  ['dpad_up', 'dpad_right'],
  ['dpad_right'],
  ['dpad_right', 'dpad_down'],
  ['dpad_down'],
  ['dpad_down', 'dpad_left'],
  ['dpad_left'],
  ['dpad_up', 'dpad_left'],
];

/// 线上真存在的 23 个位：token、63 字节状态体里的偏移、掩码。
/// 与 plugin/buttons/ids.cpp 的 kSlots 一一对应，由 tools/test_buttons_web.py 对账。
const WIRE_BITS = [
  ['triangle', 7, 0x80],
  ['circle',   7, 0x40],
  ['cross',    7, 0x20],
  ['square',   7, 0x10],
  ['l1',       8, 0x01],
  ['r1',       8, 0x02],
  ['l2',       8, 0x04],
  ['r2',       8, 0x08],
  ['create',   8, 0x10],
  ['options',  8, 0x20],
  ['l3',       8, 0x40],
  ['r3',       8, 0x80],
  ['home',     9, 0x01],
  ['pad',      9, 0x02],
  ['mute',     9, 0x04],
  ['lfn',      9, 0x10],
  ['rfn',      9, 0x20],
  ['lpaddle',  9, 0x40],
  ['rpaddle',  9, 0x80],
];

/// 摇杆方向的阈值、回迟滞与静置点（与 plugin/buttons/ids.h 同值）。
const STICK_CENTER = 128;
const STICK_STEP = 48;
const STICK_HYST = 10;

/// 触摸板几何：12 位坐标、中线与中线两侧那条“任意半区”的带（与 ids.h 同值）。
const PAD_MID_X = 2048;
const PAD_NEUTRAL_BAND = 327;

/// 两根摇杆各自的 X/Y 轴下标，与 ids::Axis 的枚举顺序同序。
const STICK_AXES = [
  {name:'lstick', x:0, y:1},
  {name:'rstick', x:2, y:3},
];

/// 手柄轮廓。取自 DualSense 线稿（daidr/dualsense-tester，MIT，© 2023 Xuezhou Dai），
/// 只换描边颜色：下缘一段、左右两片握把、肩键旁的两道小缺口。
/// 坐标就是上游的 1117×892 画布。
const PAD_SHELL = [
  'M100.97,881.749c8.551,1.607 62.356,4.094 68.38,-3.153c33.93,-40.827 69.521,-154.237 85.416,-196.14c9.791,-25.813 35.4,-37.881 67.491,-40.687c30.597,13.569 45.149,13.982 96.708,3.594l280.228,-0c51.559,10.388 66.111,9.975 96.708,-3.594c32.091,2.806 57.701,14.874 67.492,40.687c15.894,41.903 51.486,155.313 85.416,196.14c6.023,7.247 59.828,4.76 68.38,3.153',
  'M296.668,193.129c-5.479,-25.983 -25.743,-37.755 -62.282,-33.901c-40.742,4.297 -79.814,11.918 -115.424,22.309c-11.905,3.474 -23.487,8.273 -28.39,18.67c-91.01,192.963 -106.846,408.796 -55.913,643.084c2.947,13.555 9.046,24.221 21.05,32.104c5.843,3.836 12.587,7.06 21.886,9.445c21.562,5.529 31.041,-9.866 33.636,-23.758c33.416,-178.821 88.679,-342.348 191.663,-450.233c9.318,-9.761 18.181,-31.681 20.635,-51.733c0.969,-7.926 0.451,-15.333 -0.463,-22.036c-6.051,-44.414 -16.477,-96.901 -26.398,-143.951Z',
  'M820.718,193.129c5.479,-25.983 25.744,-37.755 62.283,-33.901c40.742,4.297 79.814,11.918 115.424,22.309c11.905,3.474 23.486,8.273 28.39,18.67c91.01,192.963 106.846,408.796 55.912,643.084c-2.947,13.555 -9.046,24.221 -21.05,32.104c-5.842,3.836 -12.586,7.06 -21.885,9.445c-21.563,5.529 -31.041,-9.866 -33.637,-23.758c-33.415,-178.821 -88.678,-342.348 -191.662,-450.233c-9.318,-9.761 -18.181,-31.681 -20.635,-51.733c-0.97,-7.926 -0.451,-15.333 0.462,-22.036c6.052,-44.414 16.477,-96.901 26.398,-143.951Z',
  'M282.527,168.559c13.841,1.794 27.682,-1.16 41.522,-6.511',
  'M835.33,168.559c-13.84,1.794 -27.681,-1.16 -41.522,-6.511',
];

/// 手柄图上每个线上位的形状。顺序就是绘制顺序；`icons` 是不接收指针的图形
/// （按钮上的符号），`fill:true` 表示该符号是实心的。座标全部来自同一份上游线稿。
const PAD_SHAPES = [
  {token:'l2', tag:'path', d:'M252.212,116.785l-114.214,0c-12.312,0 1.248,-110.916 78.919,-110.916c37.32,-0 51.81,110.916 35.295,110.916Z'},
  {token:'r2', tag:'path', d:'M866.471,116.785l114.214,0c12.312,0 -1.248,-110.916 -78.919,-110.916c-37.32,-0 -51.81,110.916 -35.295,110.916Z'},
  {token:'l1', tag:'path', d:'M122.514,180.513c0.736,-29.244 151.654,-71.59 148.099,-18.465c-9.518,-3.397 -21.562,-4.367 -36.227,-2.82c-39.392,4.154 -77.223,11.417 -111.872,21.285Z'},
  {token:'r1', tag:'path', d:'M995.675,180.421c-0.736,-29.244 -151.654,-71.59 -148.099,-18.465c9.518,-3.397 21.562,-4.367 36.227,-2.82c39.392,4.154 77.223,11.417 111.872,21.285Z'},
  {token:'pad', tag:'path', d:'M559.079,143.015c0,0 158.534,-0.805 226.555,15.497c12.437,2.981 21.237,14.507 19.467,24.644c-8.942,51.221 -20.354,109.033 -30.53,160.023c-8.029,40.224 -40.893,53.816 -68.431,53.692c-27.538,-0.124 -147.061,-0.559 -147.061,-0.559c0,0 -119.522,0.435 -147.06,0.559c-27.538,0.124 -60.403,-13.468 -68.431,-53.692c-10.177,-50.99 -21.589,-108.802 -30.531,-160.023c-1.77,-10.137 7.031,-21.663 19.467,-24.644c68.021,-16.302 226.555,-15.497 226.555,-15.497Z'},
  {token:'create', tag:'path', d:'M281.064,206.321l6.369,31.916c-0,-0 2.557,11.875 -11.724,14.725c-14.281,2.849 -16.478,-9.098 -16.478,-9.098l-6.4,-32.075c-0,-0 -2.559,-11.876 11.739,-14.645c14.299,-2.77 16.494,9.177 16.494,9.177Z',
   icons:[{d:'M262.694,188.058l-4.528,-7.079l4.528,7.079Z'}, {d:'M269.717,185.341l-0,-8.725l-0,8.725Z'}, {d:'M275.535,188.058l5.186,-7.079'}]},
  {token:'options', tag:'path', d:'M839.082,206.321l-6.369,31.916c-0,-0 -2.557,11.875 11.724,14.725c14.281,2.849 16.478,-9.098 16.478,-9.098l6.4,-32.075c0,-0 2.559,-11.876 -11.74,-14.645c-14.298,-2.77 -16.493,9.177 -16.493,9.177Z',
   icons:[{d:'M856.893,174.616l-13.987,0l13.987,0Z'}, {d:'M856.893,180.337l-13.987,0l13.987,0Z'}, {d:'M856.893,186.058l-13.987,-0'}]},
  // 上游那两份 id 与左右相反（几何在左臂上的那份叫 dpad-right），这里按位置取。
  {token:'dpad_up', tag:'path', d:'M213.487,271.996c0,-11.214 -9.091,-20.305 -20.305,-20.305l-28.888,-0c-11.215,-0 -20.306,9.091 -20.306,20.305l0,24.336c0,6.286 2.397,12.335 6.703,16.915c6.856,7.293 17.331,18.436 23.351,24.838c1.218,1.296 2.917,2.031 4.696,2.031c1.778,-0 3.478,-0.735 4.696,-2.031c6.019,-6.402 16.494,-17.545 23.35,-24.838c4.306,-4.58 6.703,-10.629 6.703,-16.915c0,-6.766 0,-16.013 0,-24.336Z', icons:[{d:'M168.337,270.016l18.658,0l-9.081,-9.08l-9.577,9.08Z', fill:true}]},
  {token:'dpad_right', tag:'path', d:'M263.75,322.259c11.215,0 20.306,9.091 20.306,20.306l-0,28.888c-0,11.214 -9.091,20.305 -20.306,20.305l-24.349,0c-6.278,0 -12.32,-2.391 -16.899,-6.687c-7.081,-6.644 -17.749,-16.654 -23.907,-22.433c-1.274,-1.195 -2.008,-2.855 -2.035,-4.601c-0.027,-1.746 0.656,-3.428 1.892,-4.661c6.106,-6.094 16.81,-16.778 23.946,-23.901c4.63,-4.621 10.905,-7.216 17.446,-7.216c6.724,0 15.758,0 23.906,0Z', icons:[{d:'M265.73,367.409l0,-18.657l9.081,9.08l-9.081,9.577Z', fill:true}]},
  {token:'dpad_down', tag:'path', d:'M213.487,443.093c0,11.215 -9.091,20.306 -20.305,20.306l-28.888,-0c-11.215,-0 -20.306,-9.091 -20.306,-20.306l0,-24.125c0,-6.412 2.494,-12.572 6.953,-17.178c6.884,-7.11 17.23,-17.796 23.166,-23.927c1.214,-1.254 2.885,-1.962 4.631,-1.962c1.745,0 3.416,0.708 4.63,1.962c5.936,6.131 16.282,16.817 23.166,23.927c4.46,4.606 6.953,10.766 6.953,17.178c0,6.746 0,15.888 0,24.125Z', icons:[{d:'M168.337,445.073l18.658,0l-9.081,9.08l-9.577,-9.08Z', fill:true}]},
  {token:'dpad_left', tag:'path', d:'M93.725,322.259c-11.214,0 -20.306,9.091 -20.306,20.306l0,28.888c0,11.214 9.092,20.305 20.306,20.305l24.349,0c6.278,0 12.321,-2.391 16.899,-6.687c7.081,-6.644 17.749,-16.654 23.908,-22.433c1.273,-1.195 2.007,-2.855 2.034,-4.601c0.027,-1.746 -0.656,-3.428 -1.891,-4.661c-6.106,-6.094 -16.81,-16.778 -23.947,-23.901c-4.63,-4.621 -10.904,-7.216 -17.446,-7.216c-6.724,0 -15.757,0 -23.906,0Z', icons:[{d:'M91.745,367.409l0,-18.657l-9.08,9.08l9.08,9.577Z', fill:true}]},
  {token:'triangle', tag:'circle', cx:934.079, cy:288.08, r:34.957, icons:[{d:'M934.079,271.403l20.206,33.355l-40.411,-0l20.205,-33.355Z'}]},
  {token:'circle', tag:'circle', cx:1004.08, cy:358.08, r:34.957, icons:[{tag:'circle', cx:1004.08, cy:358.08, r:19.1}]},
  {token:'cross', tag:'circle', cx:934.079, cy:428.08, r:34.957, icons:[{d:'M917.682,411.683l32.795,32.795m-0,-32.795l-32.795,32.795'}]},
  {token:'square', tag:'circle', cx:864.079, cy:358.08, r:34.957, icons:[{tag:'rect', x:847.313, y:341.648, w:32.865, h:32.865}]},
  {token:'l3', tag:'circle', cx:351.764, cy:528.548, r:57.193},
  {token:'r3', tag:'circle', cx:763.456, cy:528.548, r:57.193},
  {token:'home', tag:'bare', icons:[{d:'M525.1,538.049c-6.333,-1.817 -7.382,-5.56 -4.509,-7.734c2.071,-1.361 4.45,-2.545 6.969,-3.436l0.232,-0.072l18.72,-6.713l-0,7.71l-13.419,4.906c-2.351,0.905 -2.751,2.108 -0.798,2.754c0.974,0.248 2.089,0.392 3.239,0.392c1.641,-0 3.212,-0.291 4.666,-0.822l-0.094,0.029l6.462,-2.353l-0,6.915c-0.4,0.099 -0.854,0.147 -1.299,0.243c-2.068,0.363 -4.448,0.571 -6.875,0.571c-4.744,0 -9.3,-0.795 -13.547,-2.257l0.293,0.088l-0.04,-0.221Zm39.449,0.789l20.99,-7.584c2.383,-0.859 2.753,-2.084 0.819,-2.727c-0.968,-0.237 -2.078,-0.373 -3.222,-0.373c-1.66,-0 -3.255,0.288 -4.733,0.813l0.099,-0.029l-14.02,5l0,-7.958l0.801,-0.283c2.801,-0.931 6.093,-1.654 9.489,-2.03l0.221,-0.022c1.249,-0.133 2.695,-0.208 4.16,-0.208c5.007,0 9.809,0.883 14.254,2.505l-0.288,-0.093c6.16,2.003 6.8,4.906 5.252,6.907c-1.491,1.486 -3.294,2.662 -5.303,3.42l-0.104,0.035l-28.484,10.359l-0,-7.654l0.069,-0.078Zm-15.521,-54.146l-0,58.498l13.051,4.204l-0,-49.061c-0,-2.3 1.013,-3.836 2.646,-3.303c2.121,0.601 2.534,2.713 2.534,5.018l0,19.587c8.137,3.978 14.543,-0.008 14.543,-10.508c-0,-10.791 -3.754,-15.585 -14.796,-19.427c-4.762,-1.729 -10.778,-3.452 -16.933,-4.815l-1.037,-0.193l-0.008,0Z', fill:true}]},
  {token:'mute', tag:'path', d:'M590.061,591.266c-0,-3.699 -2.999,-6.698 -6.698,-6.698c-11.914,0 -36.653,0 -48.567,0c-3.7,0 -6.698,2.999 -6.698,6.698c-0,0.001 -0,0.001 -0,0.002c-0,3.699 2.998,6.698 6.698,6.698c11.914,-0 36.653,-0 48.567,-0c3.699,-0 6.698,-2.999 6.698,-6.698c-0,-0.001 -0,-0.001 -0,-0.002Z'},
  // Edge 只在背面多两组键；画在两只握把之间的空处，标出「背面」。
  {token:'lfn', tag:'rect', x:288, y:700, w:128, h:44, rx:22, label:'左 Fn', lx:352, ly:723},
  {token:'lpaddle', tag:'rect', x:426, y:700, w:128, h:44, rx:22, label:'左背键', lx:490, ly:723},
  {token:'rpaddle', tag:'rect', x:564, y:700, w:128, h:44, rx:22, label:'右背键', lx:628, ly:723},
  {token:'rfn', tag:'rect', x:702, y:700, w:128, h:44, rx:22, label:'右 Fn', lx:766, ly:723},
];

/// 两根摇杆四周的四个方向；判出来的形态没有线上位，所以不在 WIRE_BITS 里。
/// `hit` 是那块方向的点击范围（环外一圈，避开杆帽与握把），`d` 是环里的箭头。
const STICK_ARROWS = [
  {token:'lstick_up',    hit:[315.764,416.548,72,54], d:'M351.764,444.548 L362.764,464.548 H340.764 Z'},
  {token:'lstick_right', hit:[409.764,492.548,54,72], d:'M435.764,528.548 L415.764,517.548 V539.548 Z'},
  {token:'lstick_down',  hit:[315.764,586.548,72,54], d:'M351.764,612.548 L340.764,592.548 H362.764 Z'},
  {token:'lstick_left',  hit:[239.764,492.548,54,72], d:'M267.764,528.548 L287.764,539.548 V517.548 Z'},
  {token:'rstick_up',    hit:[727.456,416.548,72,54], d:'M763.456,444.548 L774.456,464.548 H752.456 Z'},
  {token:'rstick_right', hit:[821.456,492.548,54,72], d:'M847.456,528.548 L827.456,517.548 V539.548 Z'},
  {token:'rstick_down',  hit:[727.456,586.548,72,54], d:'M763.456,612.548 L752.456,592.548 H774.456 Z'},
  {token:'rstick_left',  hit:[651.456,492.548,54,72], d:'M679.456,528.548 L699.456,539.548 V517.548 Z'},
];

/// 触摸板那族是判出来的形态，图上没有可点的独立按钮，用图下方的 chip。
const TOUCH_TOKENS = [
  'touch_left', 'touch_right', 'touch_two',
  'pad_left', 'pad_right',
  'swipe_up', 'swipe_right', 'swipe_down', 'swipe_left',
];

let CATALOG = null;
let SOURCE_BY_TOKEN = new Map();
let GROUP_ORDER = [];

const state = {
  data: null,
  items: [],
  validation: null,
  preview: '',
  /// 壳给的按层校验入口；面板不自己发请求。
  validate: null,
  dirty: false,
  busy: false,
  error: '',
  timer: null,
  seq: 0,
};

/// 页面自己的选择：当前行与还没落到规则上的待定源。
const ui = { sel: null, pending: null };

/// 两条 WebHID 通道。real 取源，virtual 只做回显；连不上也不影响点图。
const hid = {
  real:    {device:null, handler:null, state:null, latch:new Set(), wasEngaged:false, capture:new Set(), wrong:false},
  virtual: {device:null, handler:null, state:null, latch:new Set(), wasEngaged:false, capture:new Set(), wrong:false},
};

window._kind = window._kind || 'remap';
window._q = window._q || '';
window._arm = window._arm !== false;

function setKind(id){
  window._kind = id;
  ui.sel = null;
  ui.pending = null;
  render();
}

function setSearch(value){
  window._q = value || '';
  render();
}

function setArm(on){ window._arm = Boolean(on); }

// ---------------------------------------------------------------------------
// 词表
// ---------------------------------------------------------------------------

function labelOf(token){
  const source = SOURCE_BY_TOKEN.get(token);
  return source ? source.label : token;
}

/// 允许值集合，外加"当前值"——手工写进文件里的非常规值也要能显示出来。
function allowWith(tokens, value){
  const set = new Set(tokens);
  if (value) set.add(value);
  return set;
}

/// 分组的源下拉；组名与顺序都来自服务端目录。
function sourceSelectHTML(allowed, value, handler, placeholder){
  let html = `<select onchange="__ds5Buttons.${handler}">`;
  if (placeholder) html += `<option value=""${value ? '' : ' selected'}>${esc(placeholder)}</option>`;
  for (const group of GROUP_ORDER){
    const rows = CATALOG.sources.filter(source => source.group === group && allowed.has(source.token));
    if (!rows.length) continue;
    html += `<optgroup label="${escAttr(group)}">`;
    for (const source of rows){
      html += `<option value="${escAttr(source.token)}"${source.token === value ? ' selected' : ''}>` +
              `${esc(source.label)} · ${esc(source.token)}</option>`;
    }
    html += `</optgroup>`;
  }
  return html + `</select>`;
}

function axisSelectHTML(value, handler, placeholder){
  let html = `<select onchange="__ds5Buttons.${handler}">`;
  if (placeholder) html += `<option value=""${value ? '' : ' selected'}>${esc(placeholder)}</option>`;
  for (const axis of CATALOG.axes){
    html += `<option value="${escAttr(axis.token)}"${axis.token === value ? ' selected' : ''}>` +
            `${esc(axis.label)} · ${esc(axis.token)}</option>`;
  }
  return html + `</select>`;
}

function stickSelectHTML(value, handler, placeholder){
  let html = `<select onchange="__ds5Buttons.${handler}">`;
  if (placeholder) html += `<option value=""${value ? '' : ' selected'}>${esc(placeholder)}</option>`;
  for (const stick of CATALOG.sticks){
    html += `<option value="${escAttr(stick.token)}"${stick.token === value ? ' selected' : ''}>` +
            `${esc(stick.label)} · ${esc(stick.token)}</option>`;
  }
  return html + `</select>`;
}

// ---------------------------------------------------------------------------
// 判定展示
// ---------------------------------------------------------------------------

function itemKey(item){ return item.path.join('.'); }

function entryFor(item){
  if (!state.validation || !state.validation.available) return null;
  return (state.validation.entries || []).find(entry => entry.key === itemKey(item)) || null;
}

function outcomeHTML(item){
  if (!state.validation || !state.validation.available) return '';
  const entry = entryFor(item);
  if (!entry) return `<span class="outcome" style="color:var(--sub)">不产生规则</span>`;
  if (entry.rejected) return `<span class="outcome" style="color:var(--red)">这一条不生效</span>`;
  if (entry.ignored) return `<span class="outcome" style="color:var(--warn)">插件不读这个键</span>`;
  if (entry.armed) return `<span class="outcome" style="color:var(--green)">✓ ${esc(entry.summary)}</span>`;
  return `<span class="outcome" style="color:var(--sub)">不产生规则</span>`;
}

function outcomePlain(item){
  if (!state.validation || !state.validation.available) return '校验器不可用';
  const entry = entryFor(item);
  if (!entry) return '不产生规则';
  if (entry.rejected) return '这一条不生效';
  if (entry.ignored) return '插件不读这个键';
  if (entry.armed) return '✓ ' + entry.summary;
  return '不产生规则';
}

function messageHTML(item){
  if (!state.validation || !state.validation.available) return '';
  const entry = entryFor(item);
  if (!entry || !entry.messages || !entry.messages.length) return '';
  const text = entry.messages.map(line => line.replace(/^(warning|error|debug):\s*/, '')).join('  ·  ');
  const color = entry.rejected ? 'var(--red)' : 'var(--sub)';
  return `<div style="padding:0 0 10px;font-size:12px;color:${color};line-height:1.45">${esc(text)}</div>`;
}

/// 设备条主行：整节的判定。
function validationLine(){
  if (!state.data) return '读取中…';
  if (state.error) return `<span style="color:var(--red)">${esc(state.error)}</span>`;
  const v = state.validation;
  if (!v || !v.available) return esc(v && v.error ? v.error : '校验器不可用');
  if (!v.yaml_ok) return `<span style="color:var(--red)">这一段还不能被宿主读进去：</span> ${esc(v.yaml_error)}`;
  const counts = v.rejected || v.ignored
    ? `（${v.rejected} 条不生效，${v.ignored} 条被忽略）`
    : '';
  if (v.armed) return `会生效：${esc(v.summary)}${counts}`;
  return `整节没有可解析的规则，插件不会武装${counts}`;
}

function armedNow(){
  const v = state.validation;
  return Boolean(v && v.available && v.yaml_ok && v.armed);
}

function refreshStatus(){
  const dot = document.getElementById('buttons-valdot');
  if (dot) dot.className = 'dot' + (armedNow() ? ' on' : '');
  const text = document.getElementById('buttons-valtext');
  if (text) text.innerHTML = validationLine();
}

// ---------------------------------------------------------------------------
// 变更与校验
// ---------------------------------------------------------------------------

function commit(){
  state.dirty = true;
  if (DS5.changed) DS5.changed();
  scheduleValidate();
  render();
}

function scheduleValidate(){
  clearTimeout(state.timer);
  if (!state.data || !state.data.editable || !state.data.model.structured) return;
  state.timer = setTimeout(runValidate, 220);
}

async function runValidate(){
  if (!state.data || !state.validate) return;
  const seq = ++state.seq;
  try {
    const result = await state.validate(state.items);
    if (seq !== state.seq) return;
    state.validation = result.validation;
    state.preview = result.block || '';
    state.error = '';
  } catch (error) {
    if (seq !== state.seq) return;
    state.error = error && error.message ? error.message : String(error);
  }
  const tag = (document.activeElement || {}).tagName;
  if (['INPUT', 'SELECT', 'TEXTAREA'].includes(tag)) refreshStatus();
  else render();
}

// ---------------------------------------------------------------------------
// 读写
// ---------------------------------------------------------------------------

/// 壳推过来的这一节与它的词表；面板不自己取。
function applyCtx(ctx){
  clearTimeout(state.timer);
  state.seq += 1;  // 让上一层的校验结果回来时失效
  const data = (ctx && ctx.section) || null;
  state.validate = (ctx && ctx.validate) || null;
  CATALOG = (ctx && ctx.catalog) || null;
  if (CATALOG){
    SOURCE_BY_TOKEN = new Map(CATALOG.sources.map(source => [source.token, source]));
    GROUP_ORDER = [];
    for (const source of CATALOG.sources){
      if (!GROUP_ORDER.includes(source.group)) GROUP_ORDER.push(source.group);
    }
  }
  state.data = data && !data.error ? data : null;
  state.error = data && data.error ? data.error : '';
  state.items = clone((state.data && state.data.model && state.data.model.items) || []);
  state.validation = state.data ? state.data.validation : null;
  state.preview = state.data ? state.data.block || '' : '';
  state.dirty = false;
  ui.sel = null;
  ui.pending = null;
  render();
}

// ---------------------------------------------------------------------------
// 63 字节状态体：解码、实时高亮与按住取源
// ---------------------------------------------------------------------------

/// 读状态体里的一个 32 位小端字（触摸点那两个打包字）。
function readU32(bytes, offset){
  return (bytes[offset] | (bytes[offset + 1] << 8) | (bytes[offset + 2] << 16) |
          (bytes[offset + 3] << 24)) >>> 0;
}

/// 触摸板那族的判据：半区锁存与“任意半区”的带不需要在这里复现，
/// 这里只回答“这一帧哪几个 chip 该亮”。
function touchTokens(bytes, bits){
  const out = new Set();
  let present = 0;
  for (const offset of [32, 36]){
    const word = readU32(bytes, offset);
    if (word & 0x80) continue;              // bit7 是“未接触”
    const x = (word >>> 8) & 0xfff;
    present += 1;
    if (x > PAD_MID_X + PAD_NEUTRAL_BAND) out.add('touch_right');
    else if (x + PAD_NEUTRAL_BAND < PAD_MID_X) out.add('touch_left');
  }
  if (present >= 2) out.add('touch_two');
  if (bits.has('pad')){
    if (out.has('touch_left')) out.add('pad_left');
    if (out.has('touch_right')) out.add('pad_right');
  }
  return out;
}

/**
 * 解一份 63 字节状态体。
 * @param side 哪条通道（只用它取跨帧的迟滞锁存）。
 * @param bytes 状态体。
 * @return {bits, deflection, touch}；bits 含线上位与摇杆方向。
 */
function decodeState(side, bytes){
  const st = hid[side];
  const bits = new Set();
  const deflection = new Map();
  const axis = [];
  for (let i = 0; i < 6; i += 1) axis[i] = bytes[i] || 0;

  const b0 = bytes[7] || 0;
  const hat = b0 & 0x0f;
  if (hat < 8) for (const token of HAT_TOKENS[hat]) bits.add(token);
  const slots = [b0, bytes[8] || 0, bytes[9] || 0];
  for (const [token, offset, mask] of WIRE_BITS){
    if (slots[offset - 7] & mask) bits.add(token);
  }

  // 方向位用触发线 48 判、已锁存的方向用 38 判，与 ids::StickDirections 同一条规则。
  const next = new Set();
  for (const stick of STICK_AXES){
    const x = axis[stick.x];
    const y = axis[stick.y];
    const dirs = [
      [stick.name + '_up', STICK_CENTER - y],
      [stick.name + '_right', x - STICK_CENTER],
      [stick.name + '_down', y - STICK_CENTER],
      [stick.name + '_left', STICK_CENTER - x],
    ];
    for (const [token, amount] of dirs){
      const need = st.latch.has(token) ? STICK_STEP - STICK_HYST : STICK_STEP;
      if (amount >= need){
        bits.add(token);
        deflection.set(token, amount);
        next.add(token);
      }
    }
  }
  st.latch = next;
  return {bits, deflection, touch: touchTokens(bytes, bits)};
}

/// 手柄图上所有与某一 token 对应的元素一起亮：线上位、方向箭头与 chip。
function paintLive(){
  for (const side of ['real', 'virtual']){
    const band = document.querySelector(`[data-band="${side}"]`);
    if (!band) continue;
    const current = hid[side].state;
    for (const el of band.querySelectorAll('[data-token]')){
      const token = el.dataset.token;
      const on = current && (current.bits.has(token) || current.touch.has(token));
      el.classList.toggle('live', Boolean(on));
    }
  }
  const live = document.getElementById('ctxlive');
  if (live){
    const current = hid.real.state;
    const tokens = current ? [...current.bits] : [];
    live.textContent = tokens.length ? '正在按住：' + tokens.map(labelOf).join(' + ') : '';
  }
}

/// 当前规则被选中/待定时画在图上：左边画源，右边画目标。
function paintSelection(){
  const kind = window._kind;
  const item = selectedItem();
  const top = new Set();
  const bottom = new Set();
  if (ui.pending && ui.pending.kind === 'remap' && kind === 'remap') top.add(ui.pending.src);
  if (item){
    if (kind === 'remap'){ top.add(item.src); if (item.target) bottom.add(item.target); }
    else if (kind === 'turbo'){ top.add(item.src); }
    else {
      for (const token of item.terms || []) top.add(token);
      if (kind === 'macro') for (const token of item.targets || []) bottom.add(token);
    }
  }
  for (const el of document.querySelectorAll('[data-band="real"] [data-token]')){
    el.classList.toggle('sel', top.has(el.dataset.token));
  }
  for (const el of document.querySelectorAll('[data-band="virtual"] [data-token]')){
    el.classList.toggle('sel', bottom.has(el.dataset.token));
  }
}

function selectedItem(){
  return ui.sel && ui.sel.kind === window._kind ? ui.sel : null;
}

/// 按住手柄时把当前这一组键写成源；单键规则只在按下的第一帧取一个。
function liveCapture(){
  if (!window._arm) return;
  const st = hid.real;
  if (!st.state) return;
  const kind = window._kind;
  const chord = kind === 'shortcut' || kind === 'macro';
  const engaged = st.state.bits;
  if (engaged.size === 0){
    st.capture.clear();
    st.wasEngaged = false;
    return;
  }
  if (chord){
    let changed = !st.wasEngaged;
    for (const token of engaged){
      if (!st.capture.has(token)){ st.capture.add(token); changed = true; }
    }
    st.wasEngaged = true;
    if (changed) setChordTerms(orderTokens(st.capture));
    return;
  }
  if (st.wasEngaged) return;
  st.wasEngaged = true;
  // 按目录顺序取第一个按住键；十字键的四个方向不在 WIRE_BITS 里，但它也是线上位。
  let choice = '';
  for (const source of CATALOG.sources){
    if (source.wire && engaged.has(source.token)){ choice = source.token; break; }
  }
  if (!choice){
    let best = -1;
    for (const [token, amount] of st.state.deflection){
      if (amount > best){ best = amount; choice = token; }
    }
  }
  if (choice) captureSource(choice);
}

/// 组合键与宏的源是一整组按住键：替换当前选中那条规则，没有就新建一条。
function setChordTerms(terms){
  const kind = window._kind;
  let item = selectedItem();
  if (!item){
    const id = nextRuleId(kind);
    item = {kind, path:[kind, id], id, terms: [], timing:'chord', ms: null};
    if (kind === 'macro') item.targets = ['cross'];
    state.items.push(item);
    ui.sel = item;
  }
  if (item.terms.length === terms.length && item.terms.every((token, i) => token === terms[i])) return;
  item.terms = terms;
  commit();
}

// ---------------------------------------------------------------------------
// WebHID：连接真实手柄（源）与虚拟手柄（目标回显）
// ---------------------------------------------------------------------------

function footText(side){
  const st = hid[side];
  if (st.device){
    const name = st.device.productName || 'HID 设备';
    if (st.wrong){
      const want = side === 'real' ? '0x31' : '0x01';
      return `已连接 ${name}，但没收到 ${want} 报告；可能选错了设备。`;
    }
    return `已连接 ${name}。`;
  }
  if (!navigator.hid) return '这个浏览器没有 WebHID；点图仍可配置。';
  return '未连接；点图仍可配置。';
}

function refreshBand(side){
  const band = document.querySelector(`[data-band="${side}"]`);
  if (!band) return;
  const dot = band.querySelector(`[data-dot="${side}"]`);
  if (dot) dot.className = 'dot' + (hid[side].device ? ' on' : '');
  const foot = band.querySelector(`[data-foot="${side}"]`);
  if (foot) foot.textContent = footText(side);
}

async function hidConnect(side){
  if (!navigator.hid){
    showToast('这个浏览器没有 WebHID；点图配置不受影响。', true);
    return;
  }
  let devices = [];
  try {
    devices = await navigator.hid.requestDevice({filters: [{vendorId: 0x054c}]});
  } catch (error) {
    showToast('连接失败：' + (error && error.message ? error.message : error), true);
    return;
  }
  if (!devices.length) return;
  const device = devices[0];
  try {
    if (!device.opened) await device.open();
  } catch (error) {
    showToast('打开失败：' + (error && error.message ? error.message : error) +
              '（Linux 上通常是 /dev/hidraw 权限）', true);
    return;
  }
  await hidDisconnect(side, true);
  const st = hid[side];
  st.device = device;
  st.wrong = false;
  st.handler = event => onHidReport(side, event);
  device.addEventListener('inputreport', st.handler);
  device.addEventListener('disconnect', () => {
    if (hid[side].device !== device) return;
    hidDisconnect(side);
  });
  refreshBand(side);
}

async function hidDisconnect(side, quiet){
  const st = hid[side];
  const device = st.device;
  if (device){
    if (st.handler) device.removeEventListener('inputreport', st.handler);
    try { await device.close(); } catch (error) { /* 设备已经走了 */ }
  }
  st.device = null;
  st.handler = null;
  st.state = null;
  st.wrong = false;
  st.latch.clear();
  st.capture.clear();
  st.wasEngaged = false;
  if (!quiet) paintLive();
  paintSelection();
  refreshBand(side);
}

function onHidReport(side, event){
  const st = hid[side];
  const data = event.data;
  // WebHID 把 report id 拆到 event.reportId；data 里只剩报告体。
  if (side === 'real'){
    if (event.reportId !== 0x31 || data.byteLength < 64){ st.wrong = true; refreshBand(side); return; }
    st.state = decodeState(side, new Uint8Array(data.buffer, data.byteOffset + 1, 63));
  } else {
    if (event.reportId !== 0x01 || data.byteLength < 63){ st.wrong = true; refreshBand(side); return; }
    st.state = decodeState(side, new Uint8Array(data.buffer, data.byteOffset, 63));
  }
  if (st.wrong){ st.wrong = false; refreshBand(side); }
  paintLive();
  if (side === 'real') liveCapture();
}

// ---------------------------------------------------------------------------
// 手柄示意图
// ---------------------------------------------------------------------------

/// 一个图形单元（按钮本体或它上面的符号），`cls` 决定它按哪一层样式画。
function artSVG(spec, cls){
  if (spec.tag === 'circle'){
    return `<circle class="${cls}" cx="${spec.cx}" cy="${spec.cy}" r="${spec.r}"/>`;
  }
  if (spec.tag === 'rect'){
    return `<rect class="${cls}" x="${spec.x}" y="${spec.y}" width="${spec.w}" height="${spec.h}" rx="${spec.rx || 0}"/>`;
  }
  return `<path class="${cls}" d="${spec.d}"/>`;
}

/// 一个可点的区域；`off` 是虚拟手柄上不该被当目标的方向箭头。
function shapeSVG(shape, offClass){
  const bare = shape.tag === 'bare' ? ' bare' : '';
  const attrs = `class="hit${bare}${offClass}" data-token="${shape.token}" onclick="__ds5Buttons.padClick(event)"`;
  const title = `<title>${esc(labelOf(shape.token))}</title>`;
  let inner = bare ? '' : artSVG(shape, 'shape');
  for (const icon of shape.icons || []) inner += artSVG(icon, icon.fill ? 'icon-fill' : 'icon');
  if (shape.label){
    inner += `<text class="edge-label" x="${shape.lx}" y="${shape.ly}">${esc(shape.label)}</text>`;
  }
  return `<g ${attrs}>${title}${inner}</g>`;
}

function padSVG(side){
  // 目标集合取自目录的 wire 标记：十字键四个方向也是线上位（[7] 低 nibble 的
  // hat），本地 WIRE_BITS 只负责解码，不含它们。摇杆方向与触摸板那族没有位可置。
  const targets = new Set(CATALOG.sources.filter(source => source.wire).map(source => source.token));
  const blocked = token => side === 'virtual' && !targets.has(token);
  let html = `<svg class="pad" viewBox="0 0 1117 892" role="img" aria-label="DualSense 示意图">`;
  for (const d of PAD_SHELL) html += `<path class="shell" d="${d}"/>`;
  html += `<circle class="well" cx="351.764" cy="528.548" r="87.347"/>`;
  html += `<circle class="well" cx="763.456" cy="528.548" r="87.347"/>`;
  for (const shape of PAD_SHAPES) html += shapeSVG(shape, blocked(shape.token) ? ' off' : '');
  // 方向是判出来的形态：环里画箭头，环外那块透明矩形负责接点击。
  for (const arrow of STICK_ARROWS){
    const [x, y, w, h] = arrow.hit;
    const off = blocked(arrow.token);
    html += `<g class="hit${off ? ' off' : ''}" data-token="${arrow.token}" onclick="__ds5Buttons.padClick(event)">` +
            `<title>${esc(labelOf(arrow.token))}${off ? '（摇杆方向是轴值，不能当目标）' : ''}</title>` +
            `<rect class="grab" x="${x}" y="${y}" width="${w}" height="${h}"/>` +
            `<path class="arrow" d="${arrow.d}"/></g>`;
  }
  html += `<text class="caption" x="558" y="676">背面（DualSense Edge）</text>`;
  return html + `</svg>`;
}

function padClick(event){
  const el = event.target.closest('[data-token]');
  if (!el) return;
  const band = el.closest('[data-band]');
  if (!band) return;
  if (band.dataset.band === 'real') captureSource(el.dataset.token);
  else captureTarget(el.dataset.token);
}

function chipClick(event){
  captureSource(event.currentTarget.dataset.token);
}

/// 没有线上位的源（触摸板那族）只能从这里点；按当前规则类型过滤。
function chipsHTML(){
  const kind = window._kind;
  const tokens = TOUCH_TOKENS.filter(token => {
    const source = SOURCE_BY_TOKEN.get(token);
    if (!source) return false;
    if (kind === 'remap') return source.remap_source;
    return true;
  });
  if (!tokens.length) return '';
  return `<div class="chips" style="margin-top:8px">` + tokens.map(token =>
    `<button class="chip" data-token="${escAttr(token)}" onclick="__ds5Buttons.chipClick(event)">` +
    `${esc(labelOf(token))}</button>`).join('') + `</div>`;
}

// ---------------------------------------------------------------------------
// 取源与设目标
// ---------------------------------------------------------------------------

function orderTokens(tokens){
  const index = new Map(CATALOG.sources.map((source, i) => [source.token, i]));
  return [...new Set(tokens)].sort((a, b) => {
    const left = index.has(a) ? index.get(a) : 999;
    const right = index.has(b) ? index.get(b) : 999;
    return left - right;
  });
}

function captureSource(token){
  const kind = window._kind;
  const source = SOURCE_BY_TOKEN.get(token);
  if (!source) return;

  if (kind === 'remap'){
    if (!source.remap_source){
      showToast('划动是一次性事件，不能当映射源；要它出键请配宏。', true);
      return;
    }
    const existing = state.items.find(item => item.kind === 'remap' && item.src === token);
    if (existing){
      ui.sel = existing;
      ui.pending = null;
      render();
      return;
    }
    ui.sel = null;
    ui.pending = {kind:'remap', src: token};
    render();
    return;
  }

  if (kind === 'turbo'){
    if (!source.turbo){
      showToast('连点只配在数字键上：扳机、触摸板和摇杆方向不在其中。', true);
      return;
    }
    let item = state.items.find(entry => entry.kind === 'turbo' && entry.src === token);
    if (!item){
      item = {kind:'turbo', path:['turbo', token], src: token, hz: 20};
      state.items.push(item);
      showToast(`已为 ${labelOf(token)} 新建连点规则（20 次/秒）。`, false);
    }
    ui.sel = item;
    ui.pending = null;
    commit();
    return;
  }

  // 组合键与宏：点图上的键等于往当前触发串里加/删一项。
  let item = selectedItem();
  if (!item){
    const id = nextRuleId(kind);
    item = {kind, path:[kind, id], id, terms: [], timing:'chord', ms: null};
    if (kind === 'macro') item.targets = ['cross'];
    state.items.push(item);
    ui.sel = item;
  }
  const at = item.terms.indexOf(token);
  if (at >= 0){
    if (item.terms.length <= 1){ showToast('触发至少需要一个键。', true); return; }
    item.terms.splice(at, 1);
  } else {
    item.terms = orderTokens([...item.terms, token]);
  }
  commit();
}

function captureTarget(token){
  const kind = window._kind;
  if (kind !== 'remap' && kind !== 'macro'){
    showToast('这一类没有目标键；频率/时序在下面的规则里设。', true);
    return;
  }
  if (kind === 'remap'){
    let item = selectedItem();
    if (!item && ui.pending && ui.pending.kind === 'remap'){
      item = {kind:'remap', path:['remap', ui.pending.src], src: ui.pending.src,
              target: token, keep: false, disable: false};
      state.items.push(item);
      ui.pending = null;
    } else if (!item){
      showToast('先在左边的蓝牙手柄上选一个源。', true);
      return;
    } else {
      item.target = token;
      item.disable = false;
    }
    ui.sel = item;
    commit();
    return;
  }

  let item = selectedItem();
  if (!item){
    showToast('先在左边的蓝牙手柄上选一组触发键。', true);
    return;
  }
  const at = item.targets.indexOf(token);
  if (at >= 0){
    if (item.targets.length <= 1){ showToast('宏至少要有一个目标键。', true); return; }
    item.targets.splice(at, 1);
  } else {
    item.targets = orderTokens([...item.targets, token]);
  }
  commit();
}

function selectRow(event, index){
  if (event.target.closest('input,select,button,label,a')) return;
  const item = state.items[index];
  if (!item) return;
  ui.sel = item;
  ui.pending = null;
  render();
}

// ---------------------------------------------------------------------------
// 渲染骨架
// ---------------------------------------------------------------------------

function render(){
  const root = document.getElementById('panel-buttons');
  if (!root) return;
  const active = document.activeElement;
  const focusSearch = active && active.id === 'buttons-qbox';
  const caret = focusSearch ? active.selectionStart : 0;
  root.innerHTML = panelHTML();
  if (focusSearch){
    const box = document.getElementById('buttons-qbox');
    if (box){ box.focus(); box.setSelectionRange(caret, caret); }
  }
  paintLive();
  paintSelection();
}

function panelHTML(){
  return `<section class="config-section config-section-featured" id="sec-buttons">
    <div class="config-section-heading">
      <span class="config-section-icon">${ICONS.gamepad}</span>
      <div><h3>按键映射</h3>
        <p>映射、连点、组合键与宏。</p></div>
    </div>
    ${toolbarHTML()}
    ${bodyHTML()}
  </section>`;
}

const ICONS = {
  gamepad: '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M6 11h4M8 9v4"/><circle cx="16" cy="10" r="1"/><circle cx="18" cy="12" r="1"/><path d="M17.3 5H6.7A4 4 0 0 0 2.7 8.6l-.8 7.2A3 3 0 0 0 4.9 19c.9 0 1.7-.4 2.3-1.1L9.5 15h5l2.3 2.9c.6.7 1.4 1.1 2.3 1.1a3 3 0 0 0 3-3.2l-.8-7.2A4 4 0 0 0 17.3 5Z"/></svg>',
  sliders: '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M4 21v-7M4 10V3M12 21v-9M12 8V3M20 21v-5M20 12V3"/><path d="M1 14h6M9 8h6M17 16h6"/></svg>',
  axis: '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="12" r="3"/><path d="M12 2v4M12 18v4M2 12h4M18 12h4"/></svg>',
  other: '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M8 4H6a2 2 0 0 0-2 2v3a2 2 0 0 1-2 2 2 2 0 0 1 2 2v3a2 2 0 0 0 2 2h2M16 4h2a2 2 0 0 1 2 2v3a2 2 0 0 0 2 2 2 2 0 0 0-2 2v3a2 2 0 0 1-2 2h-2"/></svg>',
  info: '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="12" r="9"/><path d="M12 11v5M12 8h.01"/></svg>',
};

function toolbarHTML(){
  return `<div class="device-strip-card"><div class="device-strip">
    <div class="device-main">
      <span class="dot${armedNow() ? ' on' : ''}" id="buttons-valdot"></span>
      <div style="min-width:0">
        <div class="device-label">插件判定</div>
        <strong id="buttons-valtext">${validationLine()}</strong>
        ${state.dirty
          ? '<div class="device-meta"><span style="color:var(--warn)">有未保存的修改</span></div>'
          : ''}
      </div>
    </div>
  </div></div>`;
}

function bodyHTML(){
  let html = '';
  if (state.error) html += `<div class="banner">${esc(state.error)}</div>`;
  if (!state.data) return html + `<div class="config-section"><p class="info-text">正在从本地服务读取配置…</p></div>`;

  const data = state.data;
  if (!data.editable){
    return html + sectionHTML('sec-mapping', ICONS.info, '这一层不可编辑', '',
      `<p class="info-text">${esc(data.reason || '')}</p>
       <pre class="yaml">${esc(data.block || '（空）')}</pre>`);
  }
  if (!data.model.structured){
    return html + sectionHTML('sec-mapping', ICONS.info, '这一节不能按行编辑', '',
      `<p class="info-text">${esc(data.model.reason || '')}</p>
       <pre class="yaml">${esc(data.block || '（空）')}</pre>`);
  }

  const query = (window._q || '').trim();
  html += searchBarHTML(query);
  if (query) return html + searchHTML(query);
  return html + mappingSectionHTML() + `<div class="config-sections">
    <div class="config-column">
      ${axisSectionHTML()}
      ${otherSectionHTML()}
    </div>
    <div class="config-column">
      ${statusSectionHTML()}
    </div>
  </div>`;
}

function sectionHTML(id, icon, title, description, body, extraClass){
  return `<section class="config-section${extraClass ? ' ' + extraClass : ''}" id="${id}">
    <div class="config-section-heading">
      <span class="config-section-icon">${icon}</span>
      <div><h3>${esc(title)}</h3>${description ? `<p>${esc(description)}</p>` : ''}</div>
    </div>
    ${body}
  </section>`;
}

function searchBarHTML(query){
  const hits = query ? searchItems(query).length : state.items.length;
  return `<div class="searchwrap">
    <input type="search" id="buttons-qbox" value="${escAttr(window._q || '')}" autocomplete="off" spellcheck="false"
           placeholder="搜索设置、键名或动作…" oninput="__ds5Buttons.setSearch(this.value)"
           onkeydown="if(event.key==='Escape')__ds5Buttons.setSearch('')">
    <span class="hits">${query ? hits + ' 条匹配' : hits + ' 条规则'}</span>
    ${query ? `<button class="btn btn-outline" style="padding:8px 14px" onclick="__ds5Buttons.setSearch('')">清除</button>` : ''}
  </div>`;
}

function statusSectionHTML(){
  const data = state.data;
  const counts = { remap:0, turbo:0, shortcut:0, macro:0, axis:0, other:0 };
  for (const item of state.items){
    if (item.kind.startsWith('axis')) counts.axis += 1;
    else if (counts[item.kind] !== undefined) counts[item.kind] += 1;
    else counts.other += 1;
  }
  const tally = [['remap','映射'],['turbo','连点'],['shortcut','组合键'],['macro','宏'],['axis','轴改写'],['other','其他']]
    .map(([key, label]) => `<b style="color:var(--text)">${counts[key]}</b> ${label}`).join(' · ');
  const info = `<dl class="kv">
    <dt>这一节</dt><dd>${data.present ? '文件里已有 buttons 配置' : '文件里还没有 buttons 配置，保存时新建'}</dd>
    <dt>规则数量</dt><dd>${tally}</dd>
  </dl>`;
  const yaml = state.preview && state.preview.trim() !== ''
    ? state.preview
    : '（这一节是空的；保存后会删掉 buttons: 这个键）';
  return sectionHTML('sec-status', ICONS.info, '这一节的 YAML',
    '保存后写进文件的部分。',
    info + `<pre class="yaml">${esc(yaml)}</pre>`);
}

// ---------------------------------------------------------------------------
// 按键映射：左右两幅图 + 上方一行规则类型
// ---------------------------------------------------------------------------

function mappingSectionHTML(){
  return sectionHTML('sec-mapping', ICONS.gamepad, '按键映射',
    '左边取源，右边设目标。',
    workbenchHTML(), 'config-section-featured');
}

function workbenchHTML(){
  return kindBarHTML() + contextHTML() +
         `<div class="padrows">${bandHTML('real')}${bandHTML('virtual')}</div>` +
         panelForKind(window._kind);
}

function kindBarHTML(){
  return `<div class="tabbar center">` + KINDS.map(kind => {
    const active = kind.id === window._kind;
    const count = state.items.filter(item => item.kind === kind.id).length;
    return `<button class="tab${active ? ' active' : ''}" onclick="__ds5Buttons.setKind('${kind.id}')">` +
           `${esc(kind.label)}<span class="count">${count}</span></button>`;
  }).join('') + `</div>`;
}

function contextLine(){
  const kind = window._kind;
  const item = selectedItem();
  if (kind === 'remap'){
    if (item){
      const target = item.disable
        ? '<b>抹掉</b>'
        : item.target ? `目标 <b>${esc(labelOf(item.target))}</b>` : '<span style="color:var(--warn)">点右边的虚拟手柄设目标</span>';
      return `源 <b>${esc(labelOf(item.src))}</b> → ${target}`;
    }
    if (ui.pending){
      return `源 <b>${esc(labelOf(ui.pending.src))}</b> → <span style="color:var(--warn)">点右边的虚拟手柄设目标</span>`;
    }
    return '按住或点左边的键取映射源。';
  }
  if (kind === 'turbo'){
    if (!item) return '按住一个键后按设定频率连点。';
    return `源 <b>${esc(labelOf(item.src))}</b> · <b>${item.hz}</b> 次/秒`;
  }
  if (!item){
    return kind === 'macro'
      ? '先按住一组触发键，再点右边的虚拟手柄选要同按的键。'
      : '先按住一组触发键。';
  }
  const terms = item.terms.map(labelOf).join(' + ');
  if (kind === 'macro'){
    return `触发 <b>${esc(terms)}</b> → 同按 <b>${esc(item.targets.map(labelOf).join(' + '))}</b>`;
  }
  return `触发 <b>${esc(terms)}</b> → <b>让手柄关机</b>`;
}

function contextHTML(){
  return `<div class="ctx" id="ctxline">${contextLine()}</div>
          <div class="ctxlive" id="ctxlive"></div>`;
}

function bandHTML(side){
  const isReal = side === 'real';
  const st = hid[side];
  const targetKind = window._kind === 'remap' || window._kind === 'macro';
  const role = isReal ? '按住或点图取源'
    : targetKind ? '点图设目标' : '这一类没有目标键';
  const muted = !isReal && !targetKind;
  return `<div class="band${muted ? ' muted' : ''}" data-band="${side}">
    <div class="band-head">
      <span class="band-title">${isReal ? '蓝牙手柄' : '虚拟手柄'}<span class="mini">${role}</span></span>
      <span class="band-tools">
        <span class="dot${st.device ? ' on' : ''}" data-dot="${side}"></span>
        ${navigator.hid ? `
          <button class="btn btn-outline" onclick="__ds5Buttons.hidConnect('${side}')">连接</button>
          <button class="btn btn-outline" onclick="__ds5Buttons.hidDisconnect('${side}')">断开</button>` : ''}
        ${isReal ? `<label class="mini"><input type="checkbox" ${window._arm ? 'checked' : ''}
          onchange="__ds5Buttons.setArm(this.checked)">按住即取源</label>` : ''}
      </span>
    </div>
    <div class="padwrap">${padSVG(side)}</div>
    ${isReal && window._kind !== 'turbo' ? chipsHTML() : ''}
    <div class="band-foot" data-foot="${side}">${esc(footText(side))}</div>
    ${!isReal && targetKind
      ? '<div class="band-foot">目标只能是按键位；摇杆方向与触摸板半区是判定出来的轴/坐标，不能当目标。</div>'
      : ''}
  </div>`;
}

function panelForKind(kind){
  if (kind === 'remap') return panelRemapHTML();
  if (kind === 'turbo') return panelTurboHTML();
  return panelRuleHTML(kind);
}

// ---------------------------------------------------------------------------
// 字段行
// ---------------------------------------------------------------------------

function addRowHTML(label, handler){
  return `<div class="addrow"><button class="btn btn-outline" onclick="__ds5Buttons.${handler}">${label}</button></div>`;
}

function rowWrap(leftHTML, rightHTML, item){
  const index = state.items.indexOf(item);
  const selected = ui.sel === item ? ' sel' : '';
  return `<div style="border-bottom:1px solid var(--border)">
    <div class="row${selected}" style="border-bottom:none;gap:10px;flex-wrap:wrap;border-radius:8px;padding:10px 8px;margin:0 -8px"
         onclick="__ds5Buttons.selectRow(event, ${index})">
      <div style="display:flex;align-items:center;gap:8px;flex-wrap:wrap;min-width:0">${leftHTML}</div>
      <div style="display:flex;align-items:center;gap:10px">${rightHTML}</div>
    </div>${messageHTML(item)}</div>`;
}

function deleteButton(i){
  return `<button class="btn btn-outline" style="padding:6px 12px" onclick="__ds5Buttons.removeItem(${i})">删除</button>`;
}

// ---------------------------------------------------------------------------
// 映射 / 连点 / 组合键 / 宏
// ---------------------------------------------------------------------------

function remapRowHTML(item, i){
  const used = new Set(state.items.filter((other, j) => j !== i && other.kind === 'remap').map(other => other.src));
  const sources = allowWith(
    CATALOG.sources.filter(source => source.remap_source && !used.has(source.token)).map(source => source.token),
    item.src);
  const targets = allowWith(
    CATALOG.sources.filter(source => source.wire).map(source => source.token),
    item.target);
  const sourcePicker = sourceSelectHTML(sources, item.src, `setRemapSrc(${i},this.value)`);
  const targetPicker = sourceSelectHTML(targets, item.target, `setRemapTarget(${i},this.value)`, '选择目标键');
  const left = `<span class="mini">源</span>${sourcePicker}
    <span style="color:var(--sub)">→</span>
    <span class="mini">目标</span>
    ${item.disable ? `<span class="chip">抹掉这个键</span>` : targetPicker}
    <label class="mini" style="display:inline-flex;align-items:center;gap:5px;cursor:pointer">
      <input type="checkbox" ${item.keep ? 'checked' : ''} ${item.disable ? 'disabled' : ''}
             onchange="__ds5Buttons.setRemapKeep(${i},this.checked)" style="width:auto;margin:0"> 保留源键</label>
    <label class="mini" style="display:inline-flex;align-items:center;gap:5px;cursor:pointer">
      <input type="checkbox" ${item.disable ? 'checked' : ''}
             onchange="__ds5Buttons.setRemapDisable(${i},this.checked)" style="width:auto;margin:0"> 抹掉</label>`;
  return rowWrap(left, outcomeHTML(item) + deleteButton(i), item);
}

function panelRemapHTML(){
  const rows = state.items.map((item, i) => [item, i]).filter(([item]) => item.kind === 'remap');
  let body = `<p class="info-text" style="margin:0 0 8px">${esc(TIPS.remap)}</p>`;
  if (!rows.length) body += `<p class="info-text">还没有映射。</p>`;
  for (const [item, i] of rows) body += remapRowHTML(item, i);
  return body + addRowHTML('+ 添加映射', 'addRemap()');
}

function turboRowHTML(item, i){
  const used = new Set(state.items.filter((other, j) => j !== i && other.kind === 'turbo').map(other => other.src));
  const sources = allowWith(
    CATALOG.sources.filter(source => source.turbo && !used.has(source.token)).map(source => source.token),
    item.src);
  const [low, high] = CATALOG.ranges.turbo;
  const left = `<span class="mini">键</span>` +
    sourceSelectHTML(sources, item.src, `setTurboSrc(${i},this.value)`) +
    `<span style="color:var(--sub)">×</span>
     <input type="number" min="${low}" max="${high}" step="1" value="${item.hz}" style="width:90px"
            onchange="__ds5Buttons.setTurboHz(${i},this.value)">
     <span class="mini">次/秒（${low}–${high}）</span>`;
  return rowWrap(left, outcomeHTML(item) + deleteButton(i), item);
}

function panelTurboHTML(){
  const rows = state.items.map((item, i) => [item, i]).filter(([item]) => item.kind === 'turbo');
  let body = `<p class="info-text" style="margin:0 0 8px">${esc(TIPS.turbo)}</p>`;
  if (!rows.length) body += `<p class="info-text">还没有连点规则。</p>`;
  for (const [item, i] of rows) body += turboRowHTML(item, i);
  return body + addRowHTML('+ 添加连点', 'addTurbo()');
}

function termsHTML(item, i){
  let chips = '';
  item.terms.forEach((term, ti) => {
    chips += `<span class="chip">${esc(labelOf(term))}<span class="tok">${esc(term)}</span>` +
             `<button title="移除" onclick="__ds5Buttons.removeTerm(${i},${ti})">×</button></span>`;
  });
  const remaining = CATALOG.sources.map(source => source.token).filter(token => !item.terms.includes(token));
  const picker = sourceSelectHTML(new Set(remaining), '', `addTerm(${i},this.value)`, '+ 添加键');
  return `<span class="chips">${chips}${picker}</span>`;
}

function timingHTML(item, i){
  let html = `<span class="mini">时序</span><select onchange="__ds5Buttons.setTiming(${i},this.value)">`;
  for (const timing of CATALOG.timings){
    html += `<option value="${timing.id}"${item.timing === timing.id ? ' selected' : ''}>${esc(timing.label)}</option>`;
  }
  html += `</select>`;
  if (item.timing === 'long'){
    const [low, high] = CATALOG.ranges.long_ms;
    html += `<input type="number" min="${low}" max="${high}" step="10" value="${item.ms || 750}" style="width:100px"
                   onchange="__ds5Buttons.setLongMs(${i},this.value)"><span class="mini">毫秒</span>`;
  }
  return html;
}

function idInputHTML(item, i){
  return `<span class="mini">#</span>
    <input type="text" value="${escAttr(item.id)}" style="width:80px" title="编号只是这条规则的标签，不参与判定"
           onchange="__ds5Buttons.setRuleId(${i},this.value)">`;
}

function targetsHTML(item, i){
  let chips = '';
  item.targets.forEach((target, ti) => {
    chips += `<span class="chip">${esc(labelOf(target))}<span class="tok">${esc(target)}</span>` +
             `<button title="移除" onclick="__ds5Buttons.removeTarget(${i},${ti})">×</button></span>`;
  });
  const remaining = CATALOG.sources.filter(source => source.wire && !item.targets.includes(source.token))
    .map(source => source.token);
  const picker = sourceSelectHTML(new Set(remaining), '', `addTarget(${i},this.value)`, '+ 添加目标');
  return `<span class="chips">${chips}${picker}</span>`;
}

function ruleRowHTML(item, i){
  const isMacro = item.kind === 'macro';
  const left = `${idInputHTML(item, i)}
    ${termsHTML(item, i)}
    ${timingHTML(item, i)}
    ${isMacro ? `<span class="mini">同按</span>${targetsHTML(item, i)}`
              : `<span class="chip">让手柄关机 · power_off</span>`}`;
  return rowWrap(left, outcomeHTML(item) + deleteButton(i), item);
}

function panelRuleHTML(group){
  const rows = state.items.map((item, i) => [item, i]).filter(([item]) => item.kind === group);
  const isMacro = group === 'macro';
  const title = isMacro ? '宏' : '组合键';
  let body = `<p class="info-text" style="margin:0 0 8px">${esc(TIPS[group])}</p>`;
  if (!rows.length) body += `<p class="info-text">还没有${title}。</p>`;
  for (const [item, i] of rows) body += ruleRowHTML(item, i);
  return body + addRowHTML(`+ 添加${title}`, `addRule('${group}')`);
}

// ---------------------------------------------------------------------------
// 轴
// ---------------------------------------------------------------------------

function axisSectionHTML(){
  const swapIndex = state.items.findIndex(item => item.kind === 'axis_swap');
  const swapValue = swapIndex >= 0 ? state.items[swapIndex].value : 'none';
  let swap = `<select onchange="__ds5Buttons.setAxisSwap(this.value)">`;
  for (const [value, label] of [['none','不交换'], ['both','整根对调（both）'], ['x','只对调 X（x）'], ['y','只对调 Y（y）']]){
    swap += `<option value="${value}"${swapValue === value ? ' selected' : ''}>${esc(label)}</option>`;
  }
  swap += `</select>`;

  let invert = '';
  for (const axis of CATALOG.axes){
    const item = state.items.find(entry => entry.kind === 'axis_invert' && entry.axis === axis.token);
    const active = Boolean(item && item.invert);
    invert += `<div class="row">
      <div class="label">${esc(axis.label)}<span class="mini"> ${esc(axis.token)}</span>${item ? messageHTML(item) : ''}</div>
      <label class="mini" style="display:inline-flex;align-items:center;gap:6px;cursor:pointer">
        <input type="checkbox" ${active ? 'checked' : ''} style="width:auto;margin:0"
               onchange="__ds5Buttons.toggleAxisInvert('${escAttr(axis.token)}',this.checked)">${active ? '反向' : '不反向'}</label>
    </div>`;
  }

  let center = '';
  for (const stick of CATALOG.sticks){
    const item = state.items.find(entry => entry.kind === 'axis_center' && entry.stick === stick.token);
    const active = Boolean(item);
    center += `<div class="row">
      <div class="label">${esc(stick.label)}<span class="mini"> center_${esc(stick.token)}</span>${item ? messageHTML(item) : ''}</div>
      <label class="mini" style="display:inline-flex;align-items:center;gap:6px;cursor:pointer">
        <input type="checkbox" ${active ? 'checked' : ''} style="width:auto;margin:0"
               onchange="__ds5Buttons.toggleAxisCenter('${escAttr(stick.token)}',this.checked)">${active ? '一直居中' : '不居中'}</label>
    </div>`;
  }

  let body = `<div class="subdiv-light">摇杆对调</div><div class="row"><div class="label">交换左右摇杆的读数${desc('对调说的是这一格读对方：both 整根对调，x / y 只换其中一对轴。')}</div><div>${swap}</div></div>`;
  body += `<div class="subdiv-light">轴反向</div>${invert}`;
  body += `<div class="subdiv-light">摇杆居中</div>${center}`;
  return sectionHTML('sec-axis', ICONS.axis, '轴改写', TIPS.axis, body);
}

// ---------------------------------------------------------------------------
// 其他
// ---------------------------------------------------------------------------

function otherRowHTML(item, i){
  const left = `<input type="text" value="${escAttr(item.path.join('.'))}" style="flex:1;min-width:170px"
                       onchange="__ds5Buttons.setOtherPath(${i},this.value)" aria-label="键路径">
    <span style="color:var(--sub)">=</span>
    <input type="text" value="${escAttr(item.value)}" style="flex:1;min-width:170px"
           onchange="__ds5Buttons.setOtherValue(${i},this.value)" aria-label="取值">`;
  return rowWrap(left, outcomeHTML(item) + deleteButton(i), item);
}

function otherSectionHTML(){
  const rows = state.items.map((item, i) => [item, i]).filter(([item]) => item.kind === 'other');
  let body = '';
  if (!rows.length) body += `<p class="info-text">没有页面不认识的写法。</p>`;
  for (const [item, i] of rows) body += otherRowHTML(item, i);
  body += addRowHTML('+ 添加其他项', 'addOther()');
  return sectionHTML('sec-other', ICONS.other, '其他', TIPS.other, body);
}

// ---------------------------------------------------------------------------
// 搜索
// ---------------------------------------------------------------------------

function haystack(item){
  const parts = [itemKey(item), item.kind];
  if (item.src) parts.push(item.src, labelOf(item.src));
  if (item.target) parts.push(item.target, labelOf(item.target));
  if (item.axis) parts.push(item.axis, labelOf(item.axis));
  if (item.stick) parts.push(item.stick, labelOf(item.stick));
  if (item.value) parts.push(item.value);
  if (item.terms) parts.push(...item.terms, ...item.terms.map(labelOf));
  if (item.targets) parts.push(...item.targets, ...item.targets.map(labelOf));
  if (item.timing) parts.push(item.timing);
  return parts.join(' ').toLowerCase();
}

function summarizeItem(item){
  switch (item.kind){
    case 'remap':
      return item.disable
        ? `${labelOf(item.src)} → 抹掉`
        : `${labelOf(item.src)} → ${labelOf(item.target)}${item.keep ? '（保留源键）' : ''}`;
    case 'turbo':
      return `${labelOf(item.src)} 连点 ${item.hz} 次/秒`;
    case 'shortcut':
    case 'macro': {
      const timing = item.timing === 'chord' ? '' : ' [' + item.timing + (item.ms ? ' ' + item.ms + 'ms' : '') + ']';
      const target = item.kind === 'macro' ? ' → ' + item.targets.map(labelOf).join('+') : ' → 关机';
      return item.terms.map(labelOf).join(' + ') + timing + target;
    }
    case 'axis_invert':
      return `${labelOf(item.axis) || item.axis} ${item.invert ? '反向' : '不反向'}`;
    case 'axis_center':
      return `${labelOf(item.stick) || item.stick} 一直居中`;
    case 'axis_swap':
      return `摇杆对调：${item.value}`;
    default:
      return `${itemKey(item)} = ${item.value}`;
  }
}

function searchItems(query){
  const needle = query.toLowerCase();
  const hits = [];
  state.items.forEach((item, index) => {
    if (haystack(item).includes(needle)) hits.push([item, index]);
  });
  return hits;
}

function sectionOf(item){
  if (item.kind === 'other') return 'sec-other';
  if (item.kind.startsWith('axis')) return 'sec-axis';
  return 'sec-mapping';
}

function searchHTML(query){
  const hits = searchItems(query);
  let body = '';
  if (!hits.length){
    body = `<p class="nohits">没有匹配的设置。试试 <b>映射</b>、<b>连点</b>、<b>宏</b>，或者某个键的名字。</p>`;
  }
  for (const [item, index] of hits){
    const kind = KINDS.find(entry => entry.id === item.kind);
    const where = kind ? kind.label : (item.kind.startsWith('axis') ? '轴' : item.kind === 'other' ? '其他' : item.kind);
    body += `<div class="row">
      <div>
        <div class="label">${esc(summarizeItem(item))}</div>
        <div class="hint">${esc(itemKey(item))} · ${esc(where)}</div>
        <div class="hint">${esc(outcomePlain(item))}</div>
      </div>
      <button class="btn btn-outline" style="padding:6px 12px" onclick="__ds5Buttons.openItem(${index})">打开</button>
    </div>`;
  }
  return sectionHTML('sec-search', ICONS.info, `搜索“${query}”`, '', body);
}

function openItem(index){
  const item = state.items[index];
  if (!item) return;
  window._q = '';
  if (KINDS.some(kind => kind.id === item.kind)){
    window._kind = item.kind;
    ui.sel = item;
    ui.pending = null;
  }
  render();
  const target = document.getElementById(sectionOf(item));
  if (target) target.scrollIntoView({behavior:'smooth', block:'start'});
}

// ---------------------------------------------------------------------------
// 各类修改入口（内联属性只传下标，模型本身留在脚本里）
// ---------------------------------------------------------------------------

function removeItem(i){
  if (i < 0 || i >= state.items.length) return;
  const removed = state.items.splice(i, 1)[0];
  if (ui.sel === removed) ui.sel = null;
  commit();
}

function setRemapSrc(i, token){
  const item = state.items[i];
  if (!item || !token) { render(); return; }
  if (state.items.some((other, j) => j !== i && other.kind === 'remap' && other.src === token)){
    showToast('这个键已经有一条映射了；一个源只留一条规则。', true);
    render();
    return;
  }
  item.src = token;
  item.path = ['remap', token];
  commit();
}

function setRemapTarget(i, token){
  const item = state.items[i];
  if (!item || !token) { render(); return; }
  item.target = token;
  commit();
}

function setRemapKeep(i, on){ if (state.items[i]){ state.items[i].keep = Boolean(on); commit(); } }
function setRemapDisable(i, on){ if (state.items[i]){ state.items[i].disable = Boolean(on); commit(); } }

function addRemap(){
  const used = new Set(state.items.filter(item => item.kind === 'remap').map(item => item.src));
  const source = CATALOG.sources.find(entry => entry.wire && !used.has(entry.token));
  if (!source){ showToast('所有可作为源的键都已经有映射了。', true); return; }
  const fallback = CATALOG.sources.find(entry => entry.wire && entry.token !== source.token);
  const item = {
    kind: 'remap',
    path: ['remap', source.token],
    src: source.token,
    target: fallback ? fallback.token : '',
    keep: false,
    disable: false,
  };
  state.items.push(item);
  ui.sel = item;
  ui.pending = null;
  commit();
}

function setTurboSrc(i, token){
  const item = state.items[i];
  if (!item || !token) { render(); return; }
  if (state.items.some((other, j) => j !== i && other.kind === 'turbo' && other.src === token)){
    showToast('这个键已经有一条连点规则了。', true);
    render();
    return;
  }
  item.src = token;
  item.path = ['turbo', token];
  commit();
}

function setTurboHz(i, raw){
  const item = state.items[i];
  if (!item) return;
  const [low, high] = CATALOG.ranges.turbo;
  let value = Number.parseInt(raw, 10);
  if (!Number.isFinite(value)) value = low;
  item.hz = Math.min(high, Math.max(low, value));
  commit();
}

function addTurbo(){
  const used = new Set(state.items.filter(item => item.kind === 'turbo').map(item => item.src));
  const source = CATALOG.sources.find(entry => entry.turbo && !used.has(entry.token));
  if (!source){ showToast('所有能连点的键都已经有规则了。', true); return; }
  const item = {kind: 'turbo', path: ['turbo', source.token], src: source.token, hz: 20};
  state.items.push(item);
  ui.sel = item;
  ui.pending = null;
  commit();
}

const TOKEN_RE = /^[A-Za-z0-9_-]{1,32}$/;

function setRuleId(i, raw){
  const item = state.items[i];
  if (!item) return;
  const next = String(raw || '').trim();
  if (!TOKEN_RE.test(next)){
    showToast('编号要用 1..32 个字母、数字、下划线或减号。', true);
    render();
    return;
  }
  if (state.items.some((other, j) => j !== i && other.kind === item.kind && other.id === next)){
    showToast('这个编号已经有一条规则了。', true);
    render();
    return;
  }
  item.id = next;
  item.path = [item.kind, next];
  commit();
}

function setTiming(i, timing){
  const item = state.items[i];
  if (!item) return;
  item.timing = timing;
  if (timing === 'tap') item.ms = CATALOG.ranges.default_tap_ms;
  else if (timing === 'double') item.ms = CATALOG.ranges.default_double_ms;
  else if (timing === 'long') item.ms = 750;
  else item.ms = null;
  commit();
}

function setLongMs(i, raw){
  const item = state.items[i];
  if (!item) return;
  const [low, high] = CATALOG.ranges.long_ms;
  let value = Number.parseInt(raw, 10);
  if (!Number.isFinite(value)) value = 750;
  item.ms = Math.min(high, Math.max(low, value));
  commit();
}

function addTerm(i, token){
  const item = state.items[i];
  if (!item || !token) return;
  if (!item.terms.includes(token)) item.terms.push(token);
  commit();
}

function removeTerm(i, index){
  const item = state.items[i];
  if (!item) return;
  if (item.terms.length <= 1){ showToast('触发至少需要一个键。', true); return; }
  item.terms.splice(index, 1);
  commit();
}

function addTarget(i, token){
  const item = state.items[i];
  if (!item || !token) return;
  if (!item.targets.includes(token)) item.targets.push(token);
  commit();
}

function removeTarget(i, index){
  const item = state.items[i];
  if (!item) return;
  if (item.targets.length <= 1){ showToast('宏至少要有一个目标键。', true); return; }
  item.targets.splice(index, 1);
  commit();
}

function nextRuleId(group){
  const used = new Set(state.items.filter(item => item.kind === group).map(item => item.id));
  for (let index = 1; index < 1000; index += 1){
    if (!used.has(String(index))) return String(index);
  }
  return String(Date.now());
}

function addRule(group){
  const id = nextRuleId(group);
  const item = {kind: group, path: [group, id], id, terms: ['l3'], timing: 'chord', ms: null};
  if (group === 'macro') item.targets = ['cross'];
  state.items.push(item);
  ui.sel = item;
  ui.pending = null;
  commit();
}

function setAxisSwap(value){
  const index = state.items.findIndex(item => item.kind === 'axis_swap');
  if (value === 'none'){
    if (index >= 0) state.items.splice(index, 1);
  } else if (index >= 0){
    state.items[index].value = value;
  } else {
    state.items.push({kind: 'axis_swap', path: ['axis', 'swap_lstick_rstick'], value});
  }
  commit();
}

function toggleAxisInvert(axis, on){
  const index = state.items.findIndex(item => item.kind === 'axis_invert' && item.axis === axis);
  if (on){
    if (index >= 0) state.items[index].invert = true;
    else state.items.push({kind: 'axis_invert', path: ['axis', 'invert_' + axis], axis, invert: true});
  } else if (index >= 0){
    state.items.splice(index, 1);
  }
  commit();
}

function toggleAxisCenter(stick, on){
  const index = state.items.findIndex(item => item.kind === 'axis_center' && item.stick === stick);
  if (on && index < 0){
    state.items.push({kind: 'axis_center', path: ['axis', 'center_' + stick], stick});
  } else if (!on && index >= 0){
    state.items.splice(index, 1);
  }
  commit();
}

function addOther(){
  const item = {kind: 'other', path: ['custom', 'key'], value: ''};
  state.items.push(item);
  ui.sel = item;
  ui.pending = null;
  commit();
}

function setOtherPath(i, raw){
  const item = state.items[i];
  if (!item) return;
  const segments = String(raw || '').split('.').map(part => part.trim());
  if (segments.length > 8 || segments.some(part => !TOKEN_RE.test(part))){
    showToast('路径最多 8 层，每层 1..32 个字母、数字、下划线或减号。', true);
    render();
    return;
  }
  item.path = segments;
  commit();
}

function setOtherValue(i, raw){
  const item = state.items[i];
  if (!item) return;
  item.value = String(raw);
  commit();
}

// ---------------------------------------------------------------------------
// 启动
// ---------------------------------------------------------------------------

/// 面板装成普通 `<script>`，与本文件同处一个全局作用域；整体包进 IIFE 是为了
/// 不和页面壳、其他面板撞名。代价是模板里的行内 `on*` 只能看到 window 上的名字，
/// 所以把模板用到的入口集中挂在一个前缀下。
window.__ds5Buttons = {
  setKind, setSearch, setArm,
  padClick, chipClick, hidConnect, hidDisconnect, selectRow, removeItem,
  setRemapSrc, setRemapTarget, addRemap, setRemapKeep, setRemapDisable,
  setTurboSrc, addTurbo, setTurboHz,
  addTerm, removeTerm, setTiming, setLongMs, setRuleId,
  addTarget, removeTarget, addRule,
  setAxisSwap, toggleAxisInvert, toggleAxisCenter,
  addOther, setOtherPath, setOtherValue, openItem,
};

document.addEventListener('keydown', event => {
  if ((event.ctrlKey || event.metaKey) && event.key === 's'){
    event.preventDefault();
    if (state.dirty) DS5.saveAll();
  }
});

DS5.define({
  id: 'buttons',
  title: '按键映射',
  async mount(host, ctx){
    applyCtx(ctx);
  },
  async refresh(ctx, force){
    if (state.dirty && !force) return;
    applyCtx(ctx);
  },
  dirty(){ return state.dirty; },
  collect(){
    if (!state.dirty || !state.data) return [];
    if (!state.data.editable || !state.data.model || !state.data.model.structured) return [];
    // 插件名与层由壳补齐；面板只交这一节的草稿。
    return [{kind: 'plugin', items: state.items}];
  },
});
})();
