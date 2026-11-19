'use strict';

const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { createHash } = require('node:crypto');
const publishRelease = require('../scripts/publish-release.cjs');

const sha = 'a'.repeat(40);
const otherSha = 'b'.repeat(40);
const tag = 'v1.1.0';
const archiveName = `cloakspace-${tag}-windows-x64.zip`;
const hash = (data) => `sha256:${createHash('sha256').update(data).digest('hex')}`;
const httpError = (status) => Object.assign(new Error(`HTTP ${status} (test fixture)`), { status });

// Deliberately in-memory GitHub double: these tests never contact GitHub or publish.
function fixture(t) {
  const build = path.resolve(__dirname, '../../build');
  fs.mkdirSync(build, { recursive: true });
  const assetDirectory = fs.mkdtempSync(path.join(build, 'release-unit-'));
  t.after(() => fs.rmSync(assetDirectory, { recursive: true, force: true }));
  function localAssets(contents = 'ZIP bytes used only by the publication unit test') {
    const data = Buffer.from(contents);
    fs.writeFileSync(path.join(assetDirectory, archiveName), data);
    fs.writeFileSync(path.join(assetDirectory, 'SHA256SUMS'), `${hash(data).slice(7)}  ${archiveName}\n`);
  }
  localAssets();
  const state = { ref: null, annotated: new Map(), release: null, otherReleases: [], assets: [], calls: [], nextId: 10, outputs: {} };
  function record(name, parameters) { state.calls.push({ name, parameters }); }
  const copy = (value) => structuredClone(value);
  const publicAsset = ({ data, ...asset }) => copy(asset);
  const releases = () => [...state.otherReleases, ...(state.release ? [state.release] : [])];
  function paginated(parameters, items) {
    assert.equal(parameters.per_page, 100);
    const page = parameters.page ?? 1;
    const start = (page - 1) * parameters.per_page;
    const end = start + parameters.per_page;
    return {
      data: copy(items.slice(start, end)),
      headers: { link: end < items.length ? `<https://api.github.invalid/releases?page=${page + 1}>; rel="next"` : '' },
    };
  }
  const git = {
    async getRef(parameters) {
      record('getRef', parameters);
      if (state.refError) throw httpError(state.refError);
      if (!state.ref) throw httpError(404);
      return { data: { object: copy(state.ref) } };
    },
    async getTag(parameters) {
      record('getTag', parameters);
      const object = state.annotated.get(parameters.tag_sha);
      if (!object) throw httpError(404);
      return { data: { object: copy(object) } };
    },
    async createRef(parameters) {
      record('createRef', parameters);
      if (state.refRace) {
        state.ref = { type: 'commit', sha: state.refRace };
        throw httpError(422);
      }
      assert.equal(state.ref, null, 'a ref must never be overwritten');
      assert.equal(parameters.ref, `refs/tags/${tag}`);
      state.ref = { type: 'commit', sha: parameters.sha };
      return { data: { object: copy(state.ref) } };
    },
  };
  const repos = {
    async getReleaseByTag(parameters) {
      record('getReleaseByTag', parameters);
      if (state.releaseError) throw httpError(state.releaseError);
      const release = releases().find((item) => !item.draft && item.tag_name === parameters.tag);
      if (!release) throw httpError(404);
      return { data: copy(release) };
    },
    async listReleases(parameters) {
      record('listReleases', parameters);
      if (state.releaseError && (!state.releaseErrorPage || parameters.page === state.releaseErrorPage)) throw httpError(state.releaseError);
      return paginated(parameters, releases());
    },
    async createRelease(parameters) {
      record('createRelease', parameters);
      assert.equal(state.release, null, 'a release must never be duplicated');
      assert.equal(parameters.draft, true, 'assets must upload before publication');
      assert.equal(parameters.prerelease, false);
      assert.equal(parameters.target_commitish, sha);
      assert.equal(parameters.make_latest, 'false');
      state.release = { ...parameters, id: 1, html_url: `https://github.invalid/test/repo/releases/tag/${tag}` };
      return { data: copy(state.release) };
    },
    async listReleaseAssets(parameters) {
      record('listReleaseAssets', parameters);
      return paginated(parameters, state.assets.map(publicAsset));
    },
    async getReleaseAsset(parameters) {
      record('getReleaseAsset', parameters);
      assert.equal(parameters.headers.accept, 'application/octet-stream');
      return { data: state.assets.find((asset) => asset.id === parameters.asset_id).data };
    },
    async deleteReleaseAsset(parameters) {
      record('deleteReleaseAsset', parameters);
      assert.equal(state.release.draft, true, 'published assets must never change');
      state.assets = state.assets.filter((asset) => asset.id !== parameters.asset_id);
      return { status: 204 };
    },
    async uploadReleaseAsset(parameters) {
      record('uploadReleaseAsset', parameters);
      assert.equal(state.release.draft, true, 'published assets must never change');
      if (state.failUpload === parameters.name) throw httpError(503);
      const data = Buffer.from(parameters.data);
      state.assets.push({ id: state.nextId++, name: parameters.name, size: data.length, digest: state.badDigest ? hash('corrupted') : hash(data), state: 'uploaded', data });
      if (state.moveTagDuringUpload) state.ref = { type: 'commit', sha: otherSha };
      return { data: publicAsset(state.assets.at(-1)) };
    },
    async getRelease(parameters) {
      record('getRelease', parameters);
      return { data: { ...copy(state.release), ...state.releaseReadback } };
    },
    async updateRelease(parameters) {
      record('updateRelease', parameters);
      assert.equal(state.assets.length, 2, 'publication requires both assets');
      assert.equal(parameters.make_latest, 'legacy');
      assert.equal(parameters.prerelease, false);
      if (!state.ignorePublication) Object.assign(state.release, parameters);
      if (state.tamperAfterPublication) state.assets[0].digest = hash('tampered after publish');
      return { data: copy(state.release) };
    },
  };
  const github = {
    rest: { git, repos },
    async paginate(method, parameters) {
      const items = [];
      for (let page = 1; ; page++) {
        const response = await method({ ...parameters, page });
        items.push(...response.data);
        if (!response.headers.link.includes('rel="next"')) return items;
      }
    },
  };
  const context = { eventName: 'push', ref: 'refs/heads/main', sha, repo: { owner: 'test', repo: 'repo' } };
  const core = { info() {}, setOutput(name, value) { state.outputs[name] = value; } };
  const run = (overrides = {}) => publishRelease({ github, context, core, tag, assetDirectory, ...overrides });
  const writes = () => state.calls.filter(({ name }) => /^(create|delete|upload|update)/.test(name));
  return { state, context, run, writes, assetDirectory, localAssets };
}

function draft(f, commit = sha) {
  f.state.ref = { type: 'commit', sha: commit };
  f.state.release = { id: 1, tag_name: tag, target_commitish: commit, draft: true, prerelease: false, html_url: 'https://github.invalid/draft' };
}

test('creates the exact commit tag, uploads both assets to a draft, verifies then publishes', async (t) => {
  const f = fixture(t);
  const result = await f.run();
  assert.equal(result.status, 'Published');
  assert.equal(result.tag, tag);
  assert.equal(f.state.ref.sha, sha);
  assert.equal(f.state.release.draft, false);
  assert.equal(f.state.release.name, `cloakspace ${tag}`);
  assert.deepEqual(f.state.assets.map((asset) => asset.name), [archiveName, 'SHA256SUMS']);
  assert.equal(f.state.outputs.url, f.state.release.html_url);
  const sequence = f.state.calls.map(({ name }) => name);
  assert.ok(sequence.indexOf('updateRelease') > sequence.lastIndexOf('uploadReleaseAsset'));
  assert.ok(sequence.lastIndexOf('getRelease') > sequence.indexOf('updateRelease'));
  assert.ok(sequence.lastIndexOf('listReleaseAssets') > sequence.indexOf('updateRelease'));
  assert.equal(f.writes().filter(({ name }) => name === 'createRelease').length, 1);
});

test('published rerun is verified and makes no writes', async (t) => {
  const f = fixture(t);
  await f.run();
  f.state.calls.length = 0;
  assert.match((await f.run()).status, /Already published/);
  assert.deepEqual(f.writes(), []);
  assert.ok(f.state.calls.some(({ name }) => name === 'getReleaseAsset'));
});

test('a later main commit with an unchanged version cannot replace published bytes or move the tag', async (t) => {
  const f = fixture(t);
  await f.run();
  const original = f.state.assets.map((asset) => asset.digest);
  f.context.sha = otherSha;
  f.localAssets('different bytes from a later commit');
  f.state.calls.length = 0;
  assert.match((await f.run()).status, /Already published/);
  assert.equal(f.state.ref.sha, sha);
  assert.deepEqual(f.state.assets.map((asset) => asset.digest), original);
  assert.deepEqual(f.writes(), []);
});

test('a same-commit tag without a release is reused', async (t) => {
  const f = fixture(t);
  f.state.ref = { type: 'commit', sha };
  await f.run();
  assert.equal(f.writes().filter(({ name }) => name === 'createRef').length, 0);
});

test('an annotated tag is resolved to the exact commit', async (t) => {
  const f = fixture(t);
  f.state.ref = { type: 'tag', sha: 'c'.repeat(40) };
  f.state.annotated.set('c'.repeat(40), { type: 'commit', sha });
  await f.run();
  assert.ok(f.state.calls.some(({ name }) => name === 'getTag'));
});

test('tag collision with another commit fails without writes', async (t) => {
  const f = fixture(t);
  f.state.ref = { type: 'commit', sha: otherSha };
  await assert.rejects(f.run(), /already points to another commit/);
  assert.deepEqual(f.writes(), []);
});

test('a draft owned by another commit is never overwritten', async (t) => {
  const f = fixture(t);
  draft(f, otherSha);
  await assert.rejects(f.run(), /Draft .* belongs to another commit/);
  assert.deepEqual(f.writes(), []);
});

test('same-commit draft without a tag ref resumes without creating a release', async (t) => {
  const f = fixture(t);
  draft(f);
  f.state.ref = null;
  const releaseId = f.state.release.id;
  assert.equal((await f.run()).status, 'Published');
  assert.equal(f.state.release.id, releaseId);
  assert.equal(f.state.release.draft, false);
  assert.equal(f.state.ref.sha, sha);
  assert.equal(f.writes().filter(({ name }) => name === 'createRef').length, 1);
  assert.ok(!f.writes().some(({ name }) => name === 'createRelease'));
});

test('foreign-commit draft without a tag ref is rejected without creating a tag', async (t) => {
  const f = fixture(t);
  draft(f, otherSha);
  f.state.ref = null;
  await assert.rejects(f.run(), /Draft .* belongs to another commit/);
  assert.equal(f.state.ref, null);
  assert.deepEqual(f.writes(), []);
});

test('multiple same-tag releases, including drafts across pages, are rejected before any writes', async (t) => {
  for (const drafts of [[true, true], [true, false], [false, false]]) {
    const f = fixture(t);
    draft(f);
    f.state.ref = null;
    f.state.release.draft = drafts[0];
    f.state.otherReleases = [
      { ...f.state.release, id: 2, draft: drafts[1] },
      ...Array.from({ length: 99 }, (_, index) => ({ id: index + 3, tag_name: `v0.0.${index}`, draft: true })),
    ];
    await assert.rejects(f.run(), /ambiguous|multiple/i);
    assert.deepEqual(f.writes(), []);
    assert.deepEqual(f.state.calls.filter(({ name }) => name === 'listReleases').map(({ parameters }) => parameters.page), [1, 2]);
  }
});

test('a matching draft after 100 unrelated releases is discovered and resumed before writes', async (t) => {
  const f = fixture(t);
  draft(f);
  f.state.ref = null;
  f.state.otherReleases = Array.from({ length: 100 }, (_, index) => ({ id: index + 2, tag_name: `v0.0.${index}`, draft: true }));
  assert.equal((await f.run()).status, 'Published');
  assert.equal(f.state.release.id, 1);
  assert.ok(!f.writes().some(({ name }) => name === 'createRelease'));
  const pages = f.state.calls.filter(({ name }) => name === 'listReleases');
  assert.deepEqual(pages.map(({ parameters }) => parameters.page), [1, 2]);
  for (const { parameters } of pages) assert.deepEqual(parameters, { ...f.context.repo, per_page: 100, page: parameters.page });
  assert.ok(f.state.calls.indexOf(f.writes()[0]) > f.state.calls.indexOf(pages.at(-1)));
});

test('release discovery failure on a later page blocks all writes', async (t) => {
  const f = fixture(t);
  f.state.otherReleases = Array.from({ length: 101 }, (_, index) => ({ id: index + 2, tag_name: `v0.0.${index}`, draft: true }));
  f.state.releaseError = 503;
  f.state.releaseErrorPage = 2;
  await assert.rejects(f.run(), /HTTP 503/);
  assert.deepEqual(f.writes(), []);
  assert.deepEqual(f.state.calls.filter(({ name }) => name === 'listReleases').map(({ parameters }) => parameters.page), [1, 2]);
});

test('same-commit partial draft resumes without duplicate release/tag', async (t) => {
  const f = fixture(t);
  f.state.failUpload = 'SHA256SUMS';
  await assert.rejects(f.run(), /HTTP 503/);
  assert.equal(f.state.release.draft, true);
  assert.equal(f.state.assets.length, 1);
  assert.ok(!f.state.calls.some(({ name }) => name === 'updateRelease'));
  f.state.failUpload = null;
  f.state.calls.length = 0;
  await f.run();
  assert.deepEqual(f.writes().map(({ name }) => name), ['uploadReleaseAsset', 'updateRelease']);
});

test('only an incomplete asset in this commit draft may be replaced', async (t) => {
  const f = fixture(t);
  draft(f);
  f.state.assets.push({ id: 2, name: archiveName, size: 0, digest: null, state: 'starter', data: Buffer.alloc(0) });
  await f.run();
  assert.equal(f.writes()[0].name, 'deleteReleaseAsset');
  assert.equal(f.state.release.draft, false);
});

test('unexpected draft assets block publication rather than being deleted', async (t) => {
  const f = fixture(t);
  draft(f);
  f.state.assets.push({ id: 2, name: 'foreign.exe', data: Buffer.from('foreign') });
  await assert.rejects(f.run(), /unexpected assets/);
  assert.deepEqual(f.writes(), []);
});

test('an incomplete published release is not silently overwritten', async (t) => {
  const f = fixture(t);
  draft(f);
  f.state.release.draft = false;
  await assert.rejects(f.run(), /missing verified assets/);
  assert.deepEqual(f.writes(), []);
});

test('published checksum corruption fails validation with no writes', async (t) => {
  const f = fixture(t);
  await f.run();
  f.state.assets.find((asset) => asset.name === 'SHA256SUMS').data = Buffer.from('wrong checksum');
  f.state.calls.length = 0;
  await assert.rejects(f.run(), /Published SHA256SUMS/);
  assert.deepEqual(f.writes(), []);
});

test('a moved published tag is detected', async (t) => {
  const f = fixture(t);
  await f.run();
  f.state.ref.sha = otherSha;
  f.state.calls.length = 0;
  await assert.rejects(f.run(), /tag\/commit is inconsistent/);
  assert.deepEqual(f.writes(), []);
});

test('prerelease metadata cannot be accepted as this regular release', async (t) => {
  const f = fixture(t);
  draft(f);
  f.state.release.prerelease = true;
  await assert.rejects(f.run(), /tag\/type/);
  assert.deepEqual(f.writes(), []);
});

test('wrong-tag metadata in release readback blocks publication', async (t) => {
  const f = fixture(t);
  draft(f);
  f.state.releaseReadback = { tag_name: 'v9.9.9' };
  await assert.rejects(f.run(), /tag\/type/);
  assert.equal(f.state.release.draft, true);
  assert.ok(!f.writes().some(({ name }) => name === 'updateRelease'));
  assert.equal(f.state.outputs.url, undefined);
});

test('invalid/missing tags and versions are rejected before any API call', async (t) => {
  for (const invalid of [undefined, '', '1.1.0', 'v1.1', 'v01.1.0', 'v1.1.0-rc1', 'v1.1.0\n', 'v65536.0.0', 'v1.1.0;echo BAD']) {
    const f = fixture(t);
    // Use null for the omitted environment variable case to avoid the default argument.
    await assert.rejects(f.run({ tag: invalid ?? null }), /Invalid .* release tag/);
    assert.deepEqual(f.state.calls, []);
  }
});

test('only exact push/main context and a full commit SHA may publish', async (t) => {
  for (const change of [{ eventName: 'pull_request' }, { ref: 'refs/heads/develop' }, { ref: 'refs/tags/v1.1.0' }, { sha: 'main' }]) {
    const f = fixture(t);
    Object.assign(f.context, change);
    await assert.rejects(f.run(), /push to main|exact built commit/);
    assert.deepEqual(f.state.calls, []);
  }
});

test('bad local checksum blocks all API calls', async (t) => {
  const f = fixture(t);
  fs.appendFileSync(path.join(f.assetDirectory, archiveName), 'corruption');
  await assert.rejects(f.run(), /SHA256SUMS does not match/);
  assert.deepEqual(f.state.calls, []);
});

test('extra or missing local assets block all API calls', async (t) => {
  for (const change of ['extra', 'missing']) {
    const f = fixture(t);
    if (change === 'extra') fs.writeFileSync(path.join(f.assetDirectory, 'debug.pdb'), 'not for release');
    else fs.unlinkSync(path.join(f.assetDirectory, archiveName));
    await assert.rejects(f.run(), /Expected only/);
    assert.deepEqual(f.state.calls, []);
  }
});

test('directories and empty files cannot masquerade as assets', async (t) => {
  for (const change of ['directory', 'empty']) {
    const f = fixture(t);
    const filename = path.join(f.assetDirectory, archiveName);
    fs.unlinkSync(filename);
    if (change === 'directory') fs.mkdirSync(filename);
    else fs.writeFileSync(filename, '');
    await assert.rejects(f.run(), /Not a regular|Empty release asset/);
    assert.deepEqual(f.state.calls, []);
  }
});

test('authentication and server failures are not treated as nonexistent tags/releases', async (t) => {
  for (const change of [{ refError: 403 }, { releaseError: 403 }, { releaseError: 503 }]) {
    const f = fixture(t);
    Object.assign(f.state, change);
    await assert.rejects(f.run(), /HTTP 403|HTTP 503/);
    assert.deepEqual(f.writes(), []);
  }
});

test('remote upload digest mismatch leaves the release as an unpublished draft', async (t) => {
  const f = fixture(t);
  f.state.badDigest = true;
  await assert.rejects(f.run(), /Unverified release asset/);
  assert.equal(f.state.release.draft, true);
  assert.ok(!f.state.calls.some(({ name }) => name === 'updateRelease'));
});

test('a tag moved during upload blocks the publication step', async (t) => {
  const f = fixture(t);
  f.state.moveTagDuringUpload = true;
  await assert.rejects(f.run(), /changed during upload/);
  assert.equal(f.state.release.draft, true);
  assert.ok(!f.state.calls.some(({ name }) => name === 'updateRelease'));
});

test('successful update response alone is not accepted without published read-back state', async (t) => {
  const f = fixture(t);
  f.state.ignorePublication = true;
  await assert.rejects(f.run(), /publication was not confirmed/);
  assert.equal(f.state.outputs.url, undefined);
});

test('post-publication read-back also verifies every asset digest', async (t) => {
  const f = fixture(t);
  f.state.tamperAfterPublication = true;
  await assert.rejects(f.run(), /Unverified release asset/);
  assert.equal(f.state.outputs.url, undefined);
});

test('a tag creation race accepts only the same exact commit', async (t) => {
  const matching = fixture(t);
  matching.state.refRace = sha;
  await matching.run();
  const conflicting = fixture(t);
  conflicting.state.refRace = otherSha;
  await assert.rejects(conflicting.run(), /HTTP 422/);
  assert.equal(conflicting.state.release, null);
});
