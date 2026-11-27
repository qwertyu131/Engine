// Keep the pinned browser inside the repo-local, git-ignored cache so the
// reference generator never depends on whatever Chrome the machine has.
const { join } = require('path');

module.exports = {
  cacheDirectory: join(__dirname, '.cache', 'puppeteer'),
};
