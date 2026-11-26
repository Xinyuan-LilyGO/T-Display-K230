# Third-Party Notices

The launcher includes emulator cores and related components under their own
copyright and license terms. This file summarizes the emulator-related terms;
the complete license texts remain next to the corresponding source files.

## ROM and game content

This repository and the default SD-card image do not include commercial game
ROMs, cartridge dumps, BIOS files, game music, game artwork, or game manuals.
The launcher only provides a local file browser and emulator runtime. Users
are responsible for obtaining and using ROM files only when they have the
necessary rights in their jurisdiction. Do not submit ROMs, commercial cover
art, or copyrighted game assets to this repository.

Console and game names are used only to identify supported file formats and
emulator targets. They do not indicate endorsement by or affiliation with the
console manufacturers or game publishers.

## Emulator components

### NES: nofrendo

- License: GNU Library General Public License, version 2 (LGPL-2.0).
- License text: `k230_launcher/k230_phone_ui/src/third_party/nofrendo/COPYING.LGPL`.
- The launcher links the core into `k230_phone_ui`. Distributors must preserve
  the LGPL notice and satisfy the applicable LGPL requirements for the exact
  binary distribution, including the ability to relink with a modified core
  where required.

### Mega Drive: Gwenesis and CPU components

- Gwenesis license: GNU Affero General Public License, version 3 (AGPL-3.0).
- License text: `k230_launcher/k230_phone_ui/src/third_party/gwenesis/LICENSE`.
- The Gwenesis integration is based on the snapshot documented in
  `k230_launcher/k230_phone_ui/src/third_party/gwenesis/README.k230.md`.
- The bundled Z80 replacement uses the zlib/libpng license in
  `k230_launcher/k230_phone_ui/src/third_party/chips/LICENSE`.
- The 68000 replacement uses the MIT license in
  `k230_launcher/k230_phone_ui/src/third_party/musashi/LICENSE`.
- The bundled SoftFloat sources carry additional notices in
  `k230_launcher/k230_phone_ui/src/third_party/musashi/softfloat/README.txt`.
- Because the core is statically linked into the launcher, distributors must
  review the AGPL corresponding-source, license-notice, and User Product
  requirements before distributing binaries or a commercial product.

### Game Boy and Game Boy Color: SameBoy

- License: Expat license (MIT-style).
- License text: `k230_launcher/k230_phone_ui/src/third_party/sameboy/LICENSE`.
- The launcher uses the portable core from SameBoy `v1.0.5`, as documented in
  `k230_launcher/k230_phone_ui/src/third_party/sameboy/README.k230.md`.
- Preserve the copyright and permission notice in source and binary
  distributions as required by that license.

### SNES: Snes9x 2005

- License: Snes9x custom non-commercial license, with GPL-covered components.
- License text: `k230_launcher/k230_phone_ui/src/third_party/snes9x/src/LICENSE`.
- The imported source snapshot and K230 changes are documented in
  `k230_launcher/k230_phone_ui/src/third_party/snes9x/README.k230.md`.
- The Snes9x license permits personal, educational, and other non-commercial
  use subject to its notice requirements. It explicitly requires prior
  permission for commercial use, including commercial products, paid bundles,
  and use as promotion for a commercial product. Do not use this core in a
  commercial product or commercial distribution without permission from the
  relevant copyright holders.

## Distribution checklist

When publishing source or a binary image:

1. Keep this file and all referenced upstream license files.
2. Keep copyright notices and identify K230 modifications.
3. Provide corresponding source and relinking materials required by LGPL or
   AGPL components for the distribution format.
4. Do not bundle or link to unauthorized commercial ROMs, BIOS files, covers,
   screenshots, music, or other game assets.
5. Recheck the Snes9x non-commercial restriction before any commercial use.

This notice is an engineering compliance summary, not legal advice. Obtain
qualified legal review for a commercial product, paid distribution, or a
distribution involving third-party ROMs or other copyrighted assets.
