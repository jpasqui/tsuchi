import starlight from '@astrojs/starlight';
import { defineConfig } from 'astro/config';
import starlightLinksValidator from 'starlight-links-validator';
import starlightSidebarTopics from 'starlight-sidebar-topics';
import { editInPlace } from './scripts/edit-in-place.mjs';
import { categories, sections } from './src/data/sections.mjs';

// The address the site is served from. Set SITE_URL when building for a host, so that the
// sitemap and the links other sites show point at the right place.
const site = process.env.SITE_URL;

// Each section's sidebar. The sections themselves are in src/data/sections.mjs.
const sidebars = {
  Players: [
    { label: 'Player Guides', slug: 'players' },
    { label: 'Install Tsuchi', slug: 'players/install-tsuchi' },
    { label: 'Install on a console', slug: 'players/install-console', badge: { text: 'WIP', variant: 'caution' } },
    { label: 'Install a mod', slug: 'players/install-a-mod' },
    { label: 'Manage mods', slug: 'players/mod-list' },
    { label: 'External mods', slug: 'players/external-mods' },
    { label: 'Troubleshooting', slug: 'players/troubleshooting' },
  ],
  Developers: [
    { label: 'Developer Guides', slug: 'develop' },
    {
      label: 'Basics',
      items: [
        { label: 'Write your first mod', slug: 'develop/first-mod' },
        { label: 'Use Lua in your mod', slug: 'develop/lua' },
        { label: 'Use JavaScript in your mod', slug: 'develop/javascript', badge: { text: 'WIP', variant: 'caution' } },
        { label: 'Write a manifest', slug: 'develop/manifest' },
        { label: 'Events and cleanup', slug: 'develop/events-and-cleanup' },
        { label: 'Logs and debugging', slug: 'develop/logs' },
        { label: 'Measure game time', slug: 'develop/time' },
        { label: 'Send network messages', slug: 'develop/network' },
        { label: 'Install packages', slug: 'develop/packages' },
        { label: 'Use IDs and kinds', slug: 'develop/ids-and-kinds' },
        { label: 'Add settings', slug: 'develop/settings' },
        { label: 'Compose mods', slug: 'develop/compose-mods' },
        { label: 'Create a screen', slug: 'develop/screens' },
        { label: 'Add menu entries', slug: 'develop/menu-entries' },
        { label: 'Save your mod data', slug: 'develop/save-data' },
        { label: 'Check compatibility', slug: 'develop/versions' },
        { label: 'Publish your mod', slug: 'develop/publish' },
        { label: 'Glossary', slug: 'develop/glossary' },
      ],
    },
    {
      label: 'Data',
      items: [
        { label: 'Change a table', slug: 'develop/change-tables' },
        { label: 'Change text', slug: 'develop/change-text' },
        { label: 'Add a treasure', slug: 'develop/add-a-treasure' },
        { label: 'Add a creature', slug: 'develop/add-a-creature' },
        { label: 'Add shader sets', slug: 'develop/add-shaders', badge: { text: 'WIP', variant: 'caution' } },
      ],
    },
    {
      label: 'Maps',
      items: [
        { label: 'Change a map', slug: 'develop/change-a-map', badge: { text: 'WIP', variant: 'caution' } },
        { label: 'Add placements', slug: 'develop/add-placements' },
        { label: 'Add a map', slug: 'develop/add-a-map', badge: { text: 'WIP', variant: 'caution' } },
      ],
    },
    {
      label: 'Gameplay',
      items: [
        { label: 'Change combat', slug: 'develop/combat' },
        { label: 'Use the squad and captain', slug: 'develop/squad-and-captain' },
        { label: 'Revive creatures', slug: 'develop/creatures' },
        { label: 'Travel between areas', slug: 'develop/story-and-travel' },
        { label: 'Ask a question', slug: 'develop/dialog-and-hud' },
        { label: 'Inspect an asset', slug: 'develop/reflection-and-assets' },
        { label: 'Read physical ground contact', slug: 'develop/ground-contact' },
        { label: 'Read carried objects', slug: 'develop/carry' },
        { label: 'Control a camera', slug: 'develop/camera-input' },
        { label: 'Use effects and navigation', slug: 'develop/effects-and-navigation' },
        { label: 'Change work objects', slug: 'develop/gimmicks' },
        { label: 'Control a battle', slug: 'develop/battle' },
        { label: 'Declare content in code', slug: 'develop/content-in-code' },
        { label: 'Add squad types', slug: 'develop/custom-squads' },
        { label: 'Add sounds', slug: 'develop/custom-sounds' },
        { label: 'Read and press buttons', slug: 'develop/input' },
      ],
    },
    {
      label: 'Native',
      items: [
        { label: 'Build a compiled mod', slug: 'develop/compiled-mod' },
        { label: 'Hook game functions', slug: 'develop/hooks' },
        { label: 'Port native hooks', slug: 'develop/native-hooks' },
      ],
    },
  ],
  Reference: [
    { label: 'Services', slug: 'reference' },
    ...categories.map((category) => ({
      label: category.label,
      items: category.services.map((name) => ({ label: name, slug: `reference/${name}` })),
    })),
  ],
};

export default defineConfig({
  ...(site ? { site } : {}),
  integrations: [
    // Editing a page's text in the browser, while `npm run dev` runs. It does nothing in a build.
    editInPlace(),
    starlight({
      title: 'Tsuchi',
      logo: { src: './src/assets/tsuchi-logo.svg', alt: '' },
      favicon: '/tsuchi-logo-favicon.svg',
      social: [{ icon: 'discord', label: 'Pikmin 4 Modding Discord', href: 'https://discord.gg/Rg5BnvuwmQ' }],
      description: 'The Pikmin 4 modding framework and API.',
      plugins: [
        // A broken link or anchor fails the build.
        starlightLinksValidator(),
        // One sidebar for each section, chosen by the page that is open.
        starlightSidebarTopics(
          sections.map((section) => ({ label: section.label, link: section.link, items: sidebars[section.label] })),
          // The first page has no sidebar and belongs to no section.
          { exclude: ['/', '/downloads'] },
        ),
      ],
      components: {
        // The site's name, followed by the links to its sections.
        SiteTitle: './src/components/SiteTitle.astro',
        // The menu on a narrow screen, on the pages that have no sidebar too.
        PageFrame: './src/components/PageFrame.astro',
        // The site builder's footer, and a line of the site's own under it.
        Footer: './src/components/Footer.astro',
      },
      customCss: [
        '@fontsource-variable/nunito',
        '@fontsource-variable/inter',
        '@fontsource-variable/jetbrains-mono',
        './src/styles/site.css',
      ],
      lastUpdated: false,
      pagination: true,
    }),
  ],
});
