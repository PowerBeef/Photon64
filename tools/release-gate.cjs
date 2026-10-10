// Shared by Actions and negative release-gate regressions.
const requiredJobs = ['baseline', 'gpu', 'safari', 'browser-chromium-default-mobile-0', 'browser-chromium-default-mobile-1', 'browser-chromium-chrome-mobile-0', 'browser-chromium-msedge-mobile-0', 'browser-webkit-default-mobile-0', 'browser-webkit-default-mobile-1'];
function verifyJobs(jobs) {
  for (const name of requiredJobs) {
    const matches = jobs.filter(j => j.name === name);
    if (matches.length !== 1 || matches[0].conclusion !== 'success') throw new Error('Required job did not pass exactly once: ' + name);
  }
  if (jobs.some(j => j.conclusion !== 'success')) throw new Error('Validation contains a failed or skipped job');
}
module.exports = { requiredJobs, verifyJobs };
