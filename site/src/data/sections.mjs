// The site's sections, and the layers the reference's services are listed under. The
// sidebar, the links at the top of every page and the reference's overview are made from this.
import { existsSync, readdirSync, readFileSync } from 'node:fs';
import path from 'node:path';

// The links at the top of every page also appear in the menu on a narrow screen.
// Each local section has a sidebar. A link that starts with `http` leads to another site.
// `icon` is a file in src/assets/icons, without `.svg`.
export const sections = [
  { label: 'Players', link: '/players/', icon: 'controller' },
  { label: 'Developers', link: '/develop/', icon: 'code' },
  { label: 'Reference', link: '/reference/', icon: 'book' },
  { label: 'Kadan', link: 'https://kadan.jpasqui.dev/', icon: 'flower' },
];

// The reference groups the services by layer. A service's layer is the `layer:` line of its
// schema file, so a new service is listed without a change here.
const layers = [
  {
    label: 'Core',
    layer: 'core',
    about:
      "Core services are part of Tsuchi itself. They cover your mod's files, settings, saved data, and timing, and they let you hook the game's functions.",
  },
  {
    label: 'Engine',
    layer: 'engine',
    about:
      'Engine services work with the parts of Unreal Engine that any Unreal game has, like assets, data tables, text, and the fields and functions of any engine object.',
  },
  {
    label: 'Game',
    layer: 'game',
    about: 'Game services are specific to Pikmin 4, like captains, Pikmin, creatures, and the story.',
  },
];

// The site's commands run in site/, and the schema is beside that folder.
const schema = ['../schema', 'schema'].map((folder) => path.resolve(process.cwd(), folder)).find(existsSync);
const layerOf = {};
for (const file of schema ? readdirSync(schema) : []) {
  if (!file.endsWith('.yaml')) continue;
  const text = readFileSync(path.join(schema, file), 'utf8');
  const service = /^service: *(\w+)/m.exec(text);
  const layer = /^layer: *(\w+)/m.exec(text);
  if (service && layer) layerOf[service[1]] = layer[1];
}

// A service whose layer is not one of the three is in no category, and the build stops on that.
export const categories = layers.map(({ label, layer, about }) => ({
  label,
  about,
  services: Object.keys(layerOf)
    .filter((name) => layerOf[name] === layer)
    .sort(),
}));
