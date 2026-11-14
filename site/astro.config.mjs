import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';

export default defineConfig({
  site: 'https://cli.gitstore.dev',
  trailingSlash: 'always',
  integrations: [starlight({
    title: 'GitStore CLI',
    description: 'GitStore from your terminal. Installation, guides, and command reference for the published alpha.',
    logo: { light: './public/branding/logo-light.svg', dark: './public/branding/logo-dark.svg', replacesTitle: false },
    favicon: '/favicon.svg',
    customCss: ['./src/styles/brand.css'],
    components: { PageTitle: './src/components/PageTitle.astro' },
    expressiveCode: {
      plugins: [{ name: 'keyboard-scrollable-code', hooks: {
        postprocessRenderedBlock: ({ renderData }) => {
          const visit = (node) => {
            if (node.type === 'element' && node.tagName === 'pre') node.properties.tabIndex = 0;
            node.children?.forEach(visit);
          };
          visit(renderData.blockAst);
        },
      } }],
    },
    social: [{ icon: 'github', label: 'Source on GitHub', href: 'https://github.com/gitstore-dev/cli' }],
    head: [
      { tag: 'meta', attrs: { property: 'og:image', content: 'https://cli.gitstore.dev/social-card.png' } },
      { tag: 'meta', attrs: { property: 'og:image:alt', content: 'GitStore CLI: your catalog, your command line. Git branches flow into catalog records.' } },
      { tag: 'meta', attrs: { property: 'og:image:width', content: '1200' } },
      { tag: 'meta', attrs: { property: 'og:image:height', content: '630' } },
      { tag: 'meta', attrs: { name: 'twitter:card', content: 'summary_large_image' } },
    ],
    sidebar: [
      { label: 'Home', link: '/' },
      { label: 'Installation', link: '/install/' },
      { label: 'Guides', items: [{ autogenerate: { directory: 'guides' } }] },
      { label: 'Command manual', items: [{ autogenerate: { directory: 'manual' } }] },
      { label: 'Contributing', link: 'https://github.com/gitstore-dev/cli/blob/main/CONTRIBUTING.md' },
    ],
  })],
});
