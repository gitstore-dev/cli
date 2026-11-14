# Asset provenance

The SVG logos and favicon are unchanged copies from `gitstore-dev/gitstore-website`
commit `88d4c402abdc277bc72c06752d6c10882e71820c`, respectively
`public/branding/logo-{light,dark}.svg` and `public/favicon.svg`.
The palette and typography in `src/styles/brand.css` derive from that commit's
`src/styles.css`, not its older Tailwind theme. No sibling checkout is used by a build.

Hero illustrations were generated with the built-in image generation tool on
2026-11-14. No generated lettering or logo is used. Selected responsive encodes
are committed in `public/art/`. The social card combines the light artwork,
unchanged SVG logo, and separately typeset text using `scripts/export-art.mjs`.
AVIF quality 45 and WebP quality 70; widths 640 and 1200. Original outputs were
1536 × 1024 PNG. Generation is not part of the website build.

Light prompt:

> Use case: stylized-concept. Asset type: branded developer documentation hero artwork, landscape 1536x1024. A restrained editorial illustration of abstract Git branches feeding catalog records. No text, no logos, no interface chrome. Thin forest-green branching paths with small orange commit nodes flow into a carefully arranged handful of cream catalog record cards with abstract lines and small botanical-green blocks. Quiet tactile cut-paper forms with subtle fine paper grain, generous negative space, precise and elegant, flat orthographic composition, no shadows heavier than soft paper layers. Warm cream #f7f6f0 background, forest #3c624d and deep ink #252922, burnt orange #c94b27 sparingly. No letters, numbers, watermarks, people, gradients, glow, 3D plastic, terminal text or invented brand marks. Legible at 600px wide and compress cleanly. Match a practical developer tool, not a busy dashboard.

Dark companion edit prompt (light artwork as the only image reference):

> Use case: style-transfer. Dark-mode companion hero illustration for developer documentation. Preserve exact composition, all branches, cards, botanical cut-paper motifs, node positions, and understated tactile paper texture. Change only the palette: backdrop deep forest #101b18, cards #22332a, branching paths light sage #bdd29f, record lines warm cream #f4f2e9, commit nodes warm orange #ff9b77. Subtle paper layers. No text, no logos, no extra objects. Keep landscape proportions and clean compression-friendly forms.
