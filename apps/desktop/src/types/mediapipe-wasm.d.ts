declare module '@mediapipe/tasks-vision/vision_wasm_module_internal.js' {
  interface MediaPipeModuleOptions {
    locateFile?: (path: string) => string;
    mainScriptUrlOrBlob?: string | Blob;
    [key: string]: unknown;
  }

  type MediaPipeModuleFactory = (options?: MediaPipeModuleOptions) => Promise<unknown>;

  const moduleFactory: MediaPipeModuleFactory;
  export default moduleFactory;
}
