import { test, expect } from '@playwright/test';
import AxeBuilder from '@axe-core/playwright';
import release from '../release.json' with { type: 'json' };

test('smoke: navigation, manual and theme preference', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByRole('heading', { level: 1 })).toContainText('Your catalog.');
  await page.getByRole('link', { name: 'Explore the manual' }).click();
  await expect(page).toHaveURL(/\/manual\/$/);
  await page.goto('/manual/auth/login/');
  await expect(page.getByRole('heading', { level: 1 })).toContainText('gs auth login');
  const theme = page.locator('starlight-theme-select select:visible');
  await theme.selectOption('light');
  await expect(page.locator('html')).toHaveAttribute('data-theme', 'light');
  await page.reload();
  await expect(page.locator('html')).toHaveAttribute('data-theme', 'light');
  await theme.selectOption('dark');
  await expect(page.locator('html')).toHaveAttribute('data-theme', 'dark');
  await theme.selectOption('auto');
});

test('search finds generated commands and authored guides', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('button', { name: 'Search', exact: true }).click();
  const search = page.locator('.pagefind-ui__search-input');
  await search.fill('auth login');
  await expect(page.locator('.pagefind-ui__result-link').first()).toBeVisible();
  await expect(page.locator('.pagefind-ui__results')).toContainText('auth');
  await search.fill('credentials');
  await expect(page.locator('.pagefind-ui__results')).toContainText(/configuration|credentials/i);
});

test('architecture selection and clipboard use published assets', async ({ page, context, browserName }) => {
  test.skip(browserName !== 'chromium');
  await context.grantPermissions(['clipboard-read', 'clipboard-write']);
  await page.goto('/');
  const deb = release.assets.find(a => a.kind === 'deb' && a.arch === 'arm64')!;
  const npm = release.assets.find(a => a.kind === 'npm' && a.os === 'darwin' && a.arch === 'arm64')!;
  await page.locator('#install-method').selectOption(deb.name);
  await page.getByRole('button', { name: 'Copy command' }).click();
  await expect(page.getByRole('status').filter({ hasText: 'Command copied.' })).toBeVisible();
  const value = await page.evaluate(() => navigator.clipboard.readText());
  expect(value).toContain(deb.url);
  expect(value).toContain(`/download/${release.tag}/`);
  expect(value).not.toMatch(/\n\+/);
  await page.locator('#install-method').selectOption(npm.name);
  await expect(page.locator('details:visible code')).toContainText('--ignore-scripts');
});

test('sessions are user controlled and reduced motion is static', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('#linux-session')).toBeHidden();
  await expect(page.locator('.step')).toHaveCount(0);
  await page.getByRole('button', { name: 'Linux deployment', exact: true }).click();
  await expect(page.locator('#linux-session')).toBeVisible();
  await page.getByRole('button', { name: 'Play example', exact: true }).click();
  await expect(page.locator('#play-session')).toHaveAttribute('aria-pressed', 'true');
  await expect(page.locator('.step')).toHaveCount(1);
  await page.getByRole('button', { name: 'Pause example', exact: true }).click();
  await expect(page.locator('.step')).toHaveCount(0);
  await page.emulateMedia({ reducedMotion: 'reduce' });
  await page.getByRole('button', { name: 'Play example', exact: true }).click();
  await expect(page.locator('.step')).toHaveCount(0);
});

for (const width of [390, 768, 1440]) {
  test(`responsive layout and focus at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 900 });
    for (const path of ['/', '/install/', '/manual/auth/login/']) {
      await page.goto(path);
      if (path === '/') {
        const theme = page.locator('starlight-theme-select select:visible');
        await theme.selectOption('dark');
        await expect(page.locator('html')).toHaveAttribute('data-theme', 'dark');
        await theme.selectOption('light');
        await expect(page.locator('html')).toHaveAttribute('data-theme', 'light');
      }
      expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBeTruthy();
      if (path === '/') await page.screenshot({ path: test.info().outputPath(`homepage-${width}.png`), fullPage: true });
      await page.keyboard.press('Tab');
      await expect(page.getByRole('link', { name: 'Skip to content' })).toBeFocused();
      await expect(page.locator(':focus')).toHaveCSS('outline-style', 'solid');
    }
  });
}

for (const theme of ['light', 'dark']) {
  test(`accessibility and contrast in ${theme} theme`, async ({ page }) => {
    for (const path of ['/', '/install/', '/manual/auth/login/']) {
      await page.goto(path);
      await page.locator('starlight-theme-select select:visible').selectOption(theme);
      const results = await new AxeBuilder({ page }).withTags(['wcag2a', 'wcag2aa', 'wcag21aa']).analyze();
      expect(results.violations).toEqual([]);
    }
  });
}

test('smoke: readable without JavaScript', async ({ browser }) => {
  const context = await browser.newContext({ javaScriptEnabled: false });
  const page = await context.newPage();
  for (const path of ['/', '/install/', '/manual/auth/login/']) {
    await page.goto(`http://127.0.0.1:4321${path}`);
    await expect(page.locator('h1')).toBeVisible();
  }
  await page.goto('http://127.0.0.1:4321/');
  await expect(page.locator('#remote-session')).toBeVisible();
  await expect(page.locator('#linux-session')).toBeVisible();
  await expect(page.getByRole('button', { name: 'Play example' })).toHaveCount(0);
  await context.close();
});

test('homepage initial resource budget stays below 500 KB', async ({ page }) => {
  const responses: Promise<number>[] = [];
  page.on('response', response => {
    if (response.url().startsWith('http://127.0.0.1:4321')) responses.push(response.body().then(body => body.length).catch(() => 0));
  });
  await page.goto('/');
  await page.waitForLoadState('networkidle');
  // Raw bodies are stricter than compressed HTTPS transfer sizes.
  const bytes = (await Promise.all(responses)).reduce((a,b) => a+b,0);
  console.log(`Initial homepage resources: ${bytes} bytes before HTTP compression`);
  expect(bytes).toBeLessThanOrEqual(500_000);
});
