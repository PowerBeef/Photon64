// Exact parity lane: diagnostic runs never count as PASS.
export function verdict(result, expected, diagnostic = false) {
  const fail = reason => ({ outcome: 'FAIL', reason });
  if (result?.error) return fail(result.error);
  if (!Array.isArray(result?.checks) || !expected.length) return fail('no checkpoints requested or returned');
  if (new Set(expected).size !== expected.length) return fail('duplicate requested checkpoints');
  const frames = result.checks.map(c => c.frame);
  if (frames.length !== expected.length || new Set(frames).size !== frames.length || expected.some(f => !frames.includes(f))) return fail('missing, duplicate, or unexpected checkpoints');
  if (diagnostic) return { outcome: 'SKIP', reason: 'diagnostic run; exact parity disabled' };
  for (const c of result.checks) {
    if (c.viDiff !== 0 || c.viMax !== 0 || c.sync !== true) return fail(`frame ${c.frame}: VI or machine-state mismatch / missing comparison`);
    if (!Number.isInteger(c.prims) || c.prims < 0 || (c.prims > 0 && !c.fb)) return fail(`frame ${c.frame}: missing rendering coverage`);
    if (c.fb) for (const kind of ['color', 'depth']) {
      if (c.fb[kind]?.dc !== 0 || c.fb[kind]?.dh !== 0) return fail(`frame ${c.frame}: ${kind} or hidden-bit mismatch / missing comparison`);
    }
  }
  return { outcome: 'PASS', reason: `${frames.length} checkpoints exact` };
}
