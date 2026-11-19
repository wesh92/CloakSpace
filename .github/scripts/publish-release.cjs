'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { createHash } = require('node:crypto');

const digest = (bytes) => `sha256:${createHash('sha256').update(bytes).digest('hex')}`;
const shaPattern = /^[0-9a-f]{40}$/;
const isSha = (value) => typeof value === 'string' && value.length === 40 && shaPattern.test(value);
const tagPattern = /^v(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)$/;

async function optional404(read) {
  try { return (await read()).data; }
  catch (error) { if (error.status === 404) return null; throw error; }
}

// Only the trusted push-to-main workflow calls this with a contents:write token.
module.exports = async function publishRelease({ github, context, core, tag = process.env.RELEASE_TAG, assetDirectory = process.env.RELEASE_ASSET_DIR }) {
  if (context.eventName !== 'push' || context.ref !== 'refs/heads/main') throw new Error('Releases require a push to main.');
  const sha = context.sha;
  if (!isSha(sha)) throw new Error('Expected the exact built commit SHA.');
  if (typeof tag !== 'string' || tag !== tag.trim() || !tagPattern.test(tag) || tag.slice(1).split('.').some((part) => Number(part) > 65535)) throw new Error('Invalid vMAJOR.MINOR.PATCH release tag.');
  const repo = context.repo;
  const archiveName = `cloakspace-${tag}-windows-x64.zip`;
  const names = [archiveName, 'SHA256SUMS'];
  const directory = path.resolve(assetDirectory);
  if (fs.readdirSync(directory).sort().join('\n') !== [...names].sort().join('\n')) throw new Error('Expected only the versioned Windows ZIP and SHA256SUMS.');
  const files = names.map((name) => {
    const filename = path.join(directory, name);
    if (!fs.lstatSync(filename).isFile()) throw new Error(`Not a regular release asset: ${name}`);
    const data = fs.readFileSync(filename);
    if (!data.length) throw new Error(`Empty release asset: ${name}`);
    return { name, data, size: data.length, digest: digest(data) };
  });
  const checksum = `${files[0].digest.slice(7)}  ${archiveName}\n`;
  if (files[1].data.toString('utf8') !== checksum) throw new Error('SHA256SUMS does not match the built ZIP.');

  async function tagCommit() {
    let object = await optional404(() => github.rest.git.getRef({ ...repo, ref: `tags/${tag}` }));
    if (!object) return null;
    object = object.object;
    for (let depth = 0; object.type === 'tag' && depth < 10; depth++) {
      object = (await github.rest.git.getTag({ ...repo, tag_sha: object.sha })).data.object;
    }
    if (object.type !== 'commit' || !isSha(object.sha)) throw new Error(`${tag} does not resolve to a commit.`);
    return object.sha;
  }
  const listAssets = (release) => github.paginate(github.rest.repos.listReleaseAssets, { ...repo, release_id: release.id, per_page: 100 });
  function verifyAssets(assets, expected = files) {
    if (assets.length !== names.length || new Set(assets.map((asset) => asset.name)).size !== names.length) throw new Error('Release must contain exactly the two expected assets.');
    for (const file of expected) {
      const asset = assets.find((item) => item.name === file.name);
      if (!asset || asset.state !== 'uploaded' || asset.size <= 0 || asset.size !== file.size || asset.digest !== file.digest) throw new Error(`Unverified release asset: ${file.name}`);
    }
  }
  function verifyRelease(release) {
    if (release.tag_name !== tag || release.prerelease) throw new Error('Release tag/type does not match the requested regular release.');
  }
  function report(release, status) {
    core.info(`${status}: ${tag} — ${release.html_url}`);
    core.setOutput('url', release.html_url);
    return { status, tag, url: release.html_url };
  }

  let target = await tagCommit();
  // The tag lookup excludes drafts; inspect every authenticated release page before any writes.
  const matches = (await github.paginate(github.rest.repos.listReleases, { ...repo, per_page: 100 })).filter((release) => release.tag_name === tag);
  if (matches.length > 1) throw new Error(`Ambiguous releases for ${tag}; resolve duplicate releases manually.`);
  let release = matches[0];
  if (release) {
    verifyRelease(release);
    if (!release.draft) {
      // Later main commits with the same version must not replace an existing release.
      if (!target || (isSha(release.target_commitish) && release.target_commitish !== target)) throw new Error('Published release tag/commit is inconsistent.');
      const assets = await listAssets(release);
      const sumAsset = assets.find((asset) => asset.name === 'SHA256SUMS');
      const zipAsset = assets.find((asset) => asset.name === archiveName);
      if (!sumAsset || !zipAsset || !/^sha256:[0-9a-f]{64}$/.test(zipAsset.digest)) throw new Error('Published release is missing verified assets; repair it manually.');
      const response = await github.rest.repos.getReleaseAsset({ ...repo, asset_id: sumAsset.id, headers: { accept: 'application/octet-stream' } });
      const sums = Buffer.from(response.data);
      if (sums.toString('utf8') !== `${zipAsset.digest.slice(7)}  ${archiveName}\n`) throw new Error('Published SHA256SUMS does not match the published ZIP.');
      verifyAssets(assets, [
        { name: archiveName, size: zipAsset.size, digest: zipAsset.digest },
        { name: 'SHA256SUMS', size: sums.length, digest: digest(sums) },
      ]);
      return report(release, 'Already published (unchanged)');
    }
    if (release.target_commitish !== sha) throw new Error(`Draft ${tag} belongs to another commit; do not overwrite it. Bump SP_VERSION or resolve the draft manually.`);
  }
  if (target && target !== sha) throw new Error(`${tag} already points to another commit; bump SP_VERSION. Tags are never moved.`);
  if (!target) {
    try { await github.rest.git.createRef({ ...repo, ref: `refs/tags/${tag}`, sha }); }
    catch (error) {
      // A concurrent/manual tag creation is safe only if it chose the same commit.
      if (error.status !== 422 || await tagCommit() !== sha) throw error;
    }
    target = await tagCommit();
    if (target !== sha) throw new Error('New release tag did not resolve to the built commit.');
  }
  if (!release) {
    release = (await github.rest.repos.createRelease({
      ...repo, tag_name: tag, target_commitish: sha, name: `cloakspace ${tag}`,
      body: `Windows x64 portable build of commit ${sha}.\n\nDownload \`${archiveName}\` and verify it with \`SHA256SUMS\`. Extract the ZIP and read README.md before running. The executable is unsigned.\n\nThe ZIP includes the sample configuration, documentation, examples and AGPL license. Corresponding source and build scripts are in the Source code archives for this exact tag. No package registry is configured.`,
      draft: true, prerelease: false, generate_release_notes: true, make_latest: 'false',
    })).data;
  }
  verifyRelease(release);
  if (!release.draft || release.target_commitish !== sha) throw new Error('Refusing to modify a published or foreign release.');
  let assets = await listAssets(release);
  if (assets.some((asset) => !names.includes(asset.name))) throw new Error('Draft contains unexpected assets; resolve it manually.');
  for (const file of files) {
    const existing = assets.find((asset) => asset.name === file.name);
    if (existing && existing.state === 'uploaded' && existing.size === file.size && existing.digest === file.digest) continue;
    // Only incomplete assets in this commit's draft may be replaced; never published assets.
    if (existing) await github.rest.repos.deleteReleaseAsset({ ...repo, asset_id: existing.id });
    await github.rest.repos.uploadReleaseAsset({
      ...repo, release_id: release.id, name: file.name, data: file.data,
      headers: { 'content-type': file.name.endsWith('.zip') ? 'application/zip' : 'text/plain', 'content-length': file.size },
    });
  }
  verifyAssets(await listAssets(release));
  release = (await github.rest.repos.getRelease({ ...repo, release_id: release.id })).data;
  verifyRelease(release);
  if (!release.draft || release.target_commitish !== sha || await tagCommit() !== sha) throw new Error('Release or tag changed during upload; refusing publication.');
  await github.rest.repos.updateRelease({ ...repo, release_id: release.id, draft: false, prerelease: false, make_latest: 'legacy' });
  // Read back the exact release, tag and full asset list before reporting success.
  release = (await github.rest.repos.getRelease({ ...repo, release_id: release.id })).data;
  verifyRelease(release);
  if (release.draft || await tagCommit() !== sha) throw new Error('Release publication was not confirmed.');
  verifyAssets(await listAssets(release));
  return report(release, 'Published');
};
