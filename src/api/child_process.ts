import type { Host } from '../host/index.js';
import type { CacheMode } from '../loader/cache.js';
import type { FSNamespace } from './fs.js';
import { ProcessImpl } from './process.js';
import type { EmX11ChildProcess, Process, SpawnOptions } from './types.js';

export class ChildProcessNamespace implements EmX11ChildProcess {
  constructor(
    private readonly host: Host,
    private readonly fs: FSNamespace,
    private readonly defaultStdout: (line: string) => void,
    private readonly defaultStderr: (line: string) => void,
    private readonly defaultCacheMode: CacheMode,
  ) {}

  spawn(glueUrl: string, options: SpawnOptions = {}): Process {
    return new ProcessImpl(
      this.host,
      this.fs,
      glueUrl,
      options,
      this.defaultStdout,
      this.defaultStderr,
      this.defaultCacheMode,
    );
  }

  async exec(glueUrl: string, options: SpawnOptions = {}): Promise<{ code: number }> {
    const p = this.spawn(glueUrl, options);
    await p.ready;
    return p.wait();
  }
}
