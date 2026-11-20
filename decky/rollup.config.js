import deckyPlugin from "@decky/rollup";

// `@decky/rollup` 负责全部约定：入口固定 `src/index.tsx`、产物落 `dist/`、
// react/@decky 走 loader 注入的全局对象（不打包进产物），`plugin.json` 的名字也由它
// 读出来给 `import.meta` 用。这里不加任何覆盖，默认配置就是本插件要的形状。
export default deckyPlugin();
